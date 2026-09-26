#include <TiltedOnlinePCH.h>

#include <Systems/InterpolationSystem.h>
#include <Components.h>

#include <AI/AIProcess.h>
#include <Misc/MiddleProcess.h>

#include <Games/References.h>
#include <Games/TES.h>
#include <Forms/TESNPC.h>
#include <PlayerCharacter.h>
#include <World.h>

#ifdef SKYRIMVR
#include <Games/Skyrim/VRBodySync.h>
#endif

void InterpolationSystem::Update(Actor* apActor, InterpolationComponent& aInterpolationComponent, const uint64_t aTick, const uint64_t aPoseTick) noexcept
{
    auto& movements = aInterpolationComponent.TimePoints;

    if (movements.size() < 2)
        return;

    while (movements.size() > 2)
    {
        const auto second = *(++movements.begin());
        if (aTick > second.Tick)
            movements.pop_front();
        else
            break;
    }

    const auto& first = *(movements.begin());
    const auto& second = *(++movements.begin());

#ifdef SKYRIMVR
    // TEMPORARY: both players lost sight of each other at 21:56 on 2026-09-18 with the actors alive and actions still
    // arriving, and a reconnect cured it, which fits a clock that drifted past the buffered movement. Every 10 s:
    // how far the newest buffered tick sits from the tick being played (negative means the actor has run out of
    // future and holds its last point), and how many actors are in that state. Remove once understood.
    {
        static int64_t s_maxAhead = INT64_MIN;
        static int64_t s_minAhead = INT64_MAX;
        static uint32_t s_updates = 0;
        static uint32_t s_starved = 0;
        static TiltedPhoques::Map<uint32_t, int64_t> s_starvedActors; // form id -> most negative "ahead" seen
        static std::chrono::steady_clock::time_point s_nextLog = std::chrono::steady_clock::now() + 10s;

        const int64_t ahead = static_cast<int64_t>(movements.back().Tick) - static_cast<int64_t>(aTick);
        s_maxAhead = std::max(s_maxAhead, ahead);
        s_minAhead = std::min(s_minAhead, ahead);
        ++s_updates;
        if (ahead < 0)
        {
            ++s_starved;
            if (apActor)
            {
                // Defaults to 0 on first sight, and ahead is negative here, so the first value always sticks.
                int64_t& worstSeen = s_starvedActors[apActor->formID];
                if (ahead < worstSeen)
                    worstSeen = ahead;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= s_nextLog)
        {
            s_nextLog = now + 10s;
            spdlog::info("InterpDiag: {} updates, newest buffered tick {} to {} ms ahead of playback, {} updates had no future point", s_updates, s_minAhead, s_maxAhead, s_starved);

            // The owner sends every actor it has every 100 ms, so an actor whose stream has stopped here is one the
            // server chose not to forward (its range check against the centre grid this client reported) or one the
            // owner no longer has. Distance in cells from this player tells which: a starved actor standing next to
            // us is the server's decision. Measured 2026-09-19: the friend had a fifth of all updates starved by up
            // to 30 s while fighting a bear the host owned.
            if (!s_starvedActors.empty())
            {
                TiltedPhoques::Vector<std::pair<uint32_t, int64_t>> worst(s_starvedActors.begin(), s_starvedActors.end());
                std::partial_sort(worst.begin(), worst.begin() + std::min<size_t>(4, worst.size()), worst.end(), [](const auto& acLhs, const auto& acRhs) { return acLhs.second < acRhs.second; });

                const PlayerCharacter* pPlayer = PlayerCharacter::Get();
                const TES* pTES = TES::Get();
                std::string line;
                for (size_t i = 0; i < worst.size() && i < 4; ++i)
                {
                    Actor* pStarved = Cast<Actor>(TESForm::GetById(worst[i].first));
                    const TESNPC* pBase = pStarved ? Cast<TESNPC>(pStarved->baseForm) : nullptr;
                    float cells = -1.f;
                    if (pStarved && pPlayer)
                        cells = glm::distance(glm::vec2{pStarved->position.x, pStarved->position.y}, glm::vec2{pPlayer->position.x, pPlayer->position.y}) / 4096.f;
                    line += fmt::format("{:X} ({}) stale {} ms, {:.1f} cells away, has 3D {}; ", worst[i].first, pBase ? pBase->fullName.value.AsAscii() : "?", -worst[i].second, cells,
                                        pStarved && pStarved->GetNiNode() != nullptr);
                }
                spdlog::info("InterpDiag starved: {} actors, worst: {}player at ({:.0f}, {:.0f}), reported centre grid ({}, {}), current grid ({}, {})", s_starvedActors.size(), line,
                             pPlayer ? pPlayer->position.x : 0.f, pPlayer ? pPlayer->position.y : 0.f, pTES ? pTES->centerGridX : 0, pTES ? pTES->centerGridY : 0,
                             pTES ? pTES->currentGridX : 0, pTES ? pTES->currentGridY : 0);
                s_starvedActors.clear();
            }

            s_maxAhead = INT64_MIN;
            s_minAhead = INT64_MAX;
            s_updates = 0;
            s_starved = 0;
        }
    }
#endif

    // Calculate delta movement since last update
    auto delta = 0.0001f;
    const auto tickDelta = static_cast<float>(second.Tick - first.Tick);
    if (tickDelta > 0.f)
    {
        delta = 1.f / tickDelta * static_cast<float>(aTick - first.Tick);
    }

    // Clamped at both ends. The lower clamp is belt and braces rather than a fix: `aTick - first.Tick` is
    // unsigned, so a playback tick behind the oldest point wraps to an enormous positive value and the upper
    // clamp already catches it. (Written on 2026-09-26 as though it fixed the displaced actors below. It does
    // not -- Lerp with delta in [0, 1] can only ever put an actor *between* the two points it is given.)
    delta = TiltedPhoques::Max(0.f, TiltedPhoques::Min(delta, 1.0f));

    const NiPoint3 position{TiltedPhoques::Lerp(first.Position, second.Position, delta)};

    aInterpolationComponent.Position = position;

    // The VR pose is played back with a shorter delay (aPoseTick) than movement: hands 300 ms behind felt
    // out of sync.
    auto& vrPose = aInterpolationComponent.InterpolatedVRPose;
    vrPose.HasData = false;
    vrPose.NoBones = false;
    vrPose.HasLegs = false;
    vrPose.HasFingers = false;
    vrPose.HasScale = false;
    vrPose.HasRootPosition = false;
    {
        const uint64_t poseTick = aPoseTick ? aPoseTick : aTick;
        const InterpolationComponent::TimePoint* pBefore = nullptr;
        const InterpolationComponent::TimePoint* pAfter = nullptr;
        for (const auto& point : movements)
        {
            if (!point.VRPoseData.HasData)
                continue;
            if (point.Tick <= poseTick)
                pBefore = &point;
            else
            {
                pAfter = &point;
                break;
            }
        }
        if (!pBefore)
            pBefore = pAfter;
        if (!pAfter)
            pAfter = pBefore;

        if (pBefore && pAfter)
        {
            float poseDelta = 1.f;
            if (pAfter->Tick > pBefore->Tick)
                poseDelta = TiltedPhoques::Min(static_cast<float>(poseTick - TiltedPhoques::Min(poseTick, pBefore->Tick)) / static_cast<float>(pAfter->Tick - pBefore->Tick), 1.0f);

            vrPose.HasData = true;
            // A body with no readable skeleton (VRPose::NoBones): there is nothing to blend and nothing to pose,
            // and the bone arrays on both points are meaningless, so they are left alone. The newest point wins,
            // as it does for every other field that is a reading rather than a rotation.
            vrPose.NoBones = pAfter->VRPoseData.NoBones || pBefore->VRPoseData.NoBones;
            if (!vrPose.NoBones)
            {
                // Legs only between two points that both carry them (the sender's trackers can come and go).
                vrPose.HasLegs = pBefore->VRPoseData.HasLegs && pAfter->VRPoseData.HasLegs;
                const size_t boneCount = vrPose.HasLegs ? VRPose::kBoneCount : VRPose::kUpperBoneCount;
                for (size_t i = 0; i < boneCount; ++i)
                    vrPose.Bones[i] = glm::slerp(static_cast<glm::quat>(pBefore->VRPoseData.Bones[i]), static_cast<glm::quat>(pAfter->VRPoseData.Bones[i]), poseDelta);
            }
            // Fingers travel only when they change, so most points carry none; the newest set around the pose tick
            // is passed on and the body sync keeps the last one it received. No blending: a grip is a step.
            const VRPose& fingerSource = pAfter->VRPoseData.HasFingers ? pAfter->VRPoseData : pBefore->VRPoseData;
            if (fingerSource.HasFingers)
            {
                vrPose.HasFingers = true;
                vrPose.Fingers = fingerSource.Fingers;
            }
            // The position of a body being dragged, blended like the movement it is. Only when both points carry
            // one, so it never blends against a stale origin.
            if (pBefore->VRPoseData.HasRootPosition && pAfter->VRPoseData.HasRootPosition)
            {
                vrPose.HasRootPosition = true;
                for (size_t i = 0; i < 3; ++i)
                    vrPose.RootPosition[i] = TiltedPhoques::Lerp(pBefore->VRPoseData.RootPosition[i], pAfter->VRPoseData.RootPosition[i], poseDelta);
            }
            else if (pAfter->VRPoseData.HasRootPosition)
            {
                vrPose.HasRootPosition = true;
                std::copy(std::begin(pAfter->VRPoseData.RootPosition), std::end(pAfter->VRPoseData.RootPosition), std::begin(vrPose.RootPosition));
            }

            const VRPose& scaleSource = pAfter->VRPoseData.HasScale ? pAfter->VRPoseData : pBefore->VRPoseData;
            if (scaleSource.HasScale)
            {
                vrPose.HasScale = true;
                vrPose.RootScale = scaleSource.RootScale;
            }

            // Hips, blended like the movement they are: the whole body is offset by this, so a step in it is a
            // step in the body. Only when both points carry one, so it never blends against a stale origin.
            //
            // This was missing until 2026-09-26, and its absence is why the hip sync shipped on 2026-09-25 never
            // did anything: the field was added to the message, to the sender and to the receiver, and not to the
            // layer in between, which rebuilds the pose from the buffer and copies only the fields it knows. A
            // new field in VRPose has to be added here too or it is silently dropped on arrival -- and the
            // receiver simply never sees it, so nothing logs and nothing complains.
            if (pBefore->VRPoseData.HasHips && pAfter->VRPoseData.HasHips)
            {
                vrPose.HasHips = true;
                for (size_t i = 0; i < 3; ++i)
                    vrPose.HipOffset[i] = TiltedPhoques::Lerp(pBefore->VRPoseData.HipOffset[i], pAfter->VRPoseData.HipOffset[i], poseDelta);
            }
            else if (pAfter->VRPoseData.HasHips)
            {
                vrPose.HasHips = true;
                std::copy(std::begin(pAfter->VRPoseData.HipOffset), std::end(pAfter->VRPoseData.HipOffset), std::begin(vrPose.HipOffset));
            }

            // The hand measurement: newest wins, no blending. It is the owner's own reading at a moment in time
            // and averaging two of them would make it agree with nothing.
            const VRPose& handSource = pAfter->VRPoseData.HasHandCheck ? pAfter->VRPoseData : pBefore->VRPoseData;
            if (handSource.HasHandCheck)
            {
                vrPose.HasHandCheck = true;
                std::copy(std::begin(handSource.LeftHandOffset), std::end(handSource.LeftHandOffset), std::begin(vrPose.LeftHandOffset));
                std::copy(std::begin(handSource.RightHandOffset), std::end(handSource.RightHandOffset), std::begin(vrPose.RightHandOffset));
            }
        }
    }

#ifdef SKYRIMVR
    VRBodySync::SetRemotePose(apActor, vrPose);
#endif

    if (!apActor)
        return;

    // A dying body falls with its own ragdoll. Once dead it is moved to where the owner's corpse lies, but only
    // when it is clearly elsewhere, so a settled ragdoll isn't pulled around every frame.
    // A dying body falls with its own ragdoll, and a downed one has to be able to get back up where it fell;
    // forcing either to follow its owner's position pulls the body away from the animation playing on it.
    if (apActor->actorState.IsDying() || apActor->actorState.IsBleedingOut())
        return;

    if (apActor->actorState.IsDead())
    {
        constexpr float cCorpseSnapDistance = 64.f;
        const glm::vec3 current{apActor->position.x, apActor->position.y, apActor->position.z};
        if (glm::distance(current, position) > cCorpseSnapDistance)
            apActor->ForcePosition(position);
        return;
    }

#ifdef SKYRIMVR
    // TEMPORARY: remote NPCs were seen standing under the ground on one side only (2026-09-18). Measures how far the
    // game moved a remote actor down between two placements, so the log tells whether it is placed too low or sinks
    // afterwards. Remove once understood.
    {
        static TiltedPhoques::Map<uint32_t, float> s_lastPlacedZ;
        static float s_worstSink = 0.f;
        static uint32_t s_sunkPlacements = 0;
        static uint32_t s_worstActor = 0;
        static std::chrono::steady_clock::time_point s_nextLog = std::chrono::steady_clock::now() + 10s;

        // Only while the points being interpolated between are current. On 2026-09-23 at 19:04:11 this reported
        // 195 sinks with a worst of 875,458 units -- two hundred cells, which is not a body sinking through a
        // floor but the buffer being fifteen seconds behind playback ("newest buffered tick -15575"). Positions
        // interpolated across a backlog like that are nonsense, and counting them here buries the real signal,
        // which is the ordinary 30-to-190-unit drops that show up one or two at a time.
        const uint64_t cNewestTick = movements.back().Tick;
        const bool cFresh = cNewestTick + 1000 >= aTick;

        if (const auto it = s_lastPlacedZ.find(apActor->formID); cFresh && it != s_lastPlacedZ.end())
        {
            const float sink = it->second - apActor->position.z;
            if (sink > 16.f)
            {
                ++s_sunkPlacements;
                if (sink > s_worstSink)
                {
                    s_worstSink = sink;
                    s_worstActor = apActor->formID;
                }
            }
        }
        s_lastPlacedZ[apActor->formID] = position.z;

        const auto now = std::chrono::steady_clock::now();
        if (now >= s_nextLog)
        {
            s_nextLog = now + 10s;
            if (s_sunkPlacements)
            {
                // "has controller" was added on 2026-09-26. An actor that sinks *with* a controller is physics
                // disagreeing about the floor; one that sinks without is a body nothing is holding up, which
                // points straight at ForcePosition giving up on the controller (see ControllerDiag).
                const Actor* pWorst = s_worstActor ? Cast<Actor>(TESForm::GetById(s_worstActor)) : nullptr;
                spdlog::info("SinkDiag: {} placements found a remote actor lower than where it was last put, worst {:.0f} units on {:X}, has controller {}", s_sunkPlacements, s_worstSink,
                             s_worstActor, pWorst && pWorst->currentProcess && pWorst->currentProcess->GetCharController() ? "yes" : "no");
            }
            s_sunkPlacements = 0;
            s_worstSink = 0.f;
            s_worstActor = 0;
        }
    }
#endif

// MotionDiag lived here and has been removed (2026-09-25). It answered its question -- remote NPCs slid because the
// wrong function was skipped on VR -- and every session since has reported "0 with bones that had not changed".
// What it cost to keep asking: for every moving remote NPC, every fourth frame, a walk of the whole skeleton into a
// freshly allocated vector and a 4 KB hash. With twenty NPCs about that is several skeleton walks and allocations
// per frame, for an answer already known. Stalls are what time a client out, and a timeout hands its actors away.


#ifdef SKYRIMVR
    // Sliding, measured the cheap way (2026-09-26). Emma and Seen saw each other sliding for half a session.
    //
    // MotionDiag used to answer this by walking the whole skeleton and hashing it, and I deleted it on
    // 2026-09-26 on the grounds that its question was settled -- the same day the symptom came back. That was
    // premature. This asks the same question for a fraction of the cost: a body that translates while the
    // animation variables driving its legs do not change is a body sliding, and both numbers are already here.
    {
        static uint32_t s_moved = 0;
        static uint32_t s_movedFrozen = 0;
        static uint32_t s_worstActor = 0;
        static std::chrono::steady_clock::time_point s_nextLog = std::chrono::steady_clock::now() + 10s;

        if (glm::distance(static_cast<glm::vec3>(apActor->position), position) > 2.f)
        {
            ++s_moved;
            if (first.Variables == second.Variables)
            {
                ++s_movedFrozen;
                s_worstActor = apActor->formID;
            }
        }

        const auto slideNow = std::chrono::steady_clock::now();
        if (slideNow >= s_nextLog)
        {
            s_nextLog = slideNow + 10s;
            if (s_moved)
                spdlog::info("SlideDiag: {} of {} moves of a remote body came with animation variables that had not changed; last was {:X}. All of them would be a body sliding rather than walking.",
                             s_movedFrozen, s_moved, s_worstActor);
            s_moved = 0;
            s_movedFrozen = 0;
            s_worstActor = 0;
        }
    }
#endif

    apActor->ForcePosition(position);
    // A creature of another kind than the owner's (see MarkForeignGraph) keeps its own animation state.
    if (!aInterpolationComponent.ForeignGraph)
        apActor->LoadAnimationVariables(second.Variables);

    if (apActor->currentProcess && apActor->currentProcess->middleProcess)
    {
        apActor->currentProcess->middleProcess->direction = second.Direction;
    }

    auto rotA = first.Rotation;
    auto rotB = second.Rotation;

    const auto deltaX = TiltedPhoques::DeltaAngle(rotA.x, rotB.x, true) * delta;
    const auto deltaY = TiltedPhoques::DeltaAngle(rotA.y, rotB.y, true) * delta;
    const auto deltaZ = TiltedPhoques::DeltaAngle(rotA.z, rotB.z, true) * delta;

    auto finalX = TiltedPhoques::Mod(rotA.x + deltaX, float(TiltedPhoques::Pi * 2));
    if (finalX > 0.f && finalX > float(TiltedPhoques::Pi / 2))
        finalX -= TiltedPhoques::Pi * 2;

    const auto finalY = TiltedPhoques::Mod(rotA.y + deltaY, float(TiltedPhoques::Pi * 2));
    const auto finalZ = TiltedPhoques::Mod(rotA.z + deltaZ, float(TiltedPhoques::Pi * 2));

    apActor->SetRotation(finalX, finalY, finalZ);
}

void InterpolationSystem::AddPoint(InterpolationComponent& aInterpolationComponent, const InterpolationComponent::TimePoint& acPoint) noexcept
{
    // Two points far enough apart that no actor could have travelled between them do not describe a journey, and
    // interpolating across them drags the actor over everything in between. The buffer keeps no worldspace, so a
    // point from before a worldspace change and one from after sit side by side and get walked between -- which
    // is Solstheim to somewhere else, hundreds of cells, in a tenth of a second.
    //
    // That is the 875,458 units SinkDiag reported on 2026-09-23 with 195 actors displaced at once, and it is
    // what "a bandit got launched into the sky" looks like from inside the game. A cell is 4096 units and the
    // points are about a tenth of a second apart, so anything approaching that is not movement: drop what is
    // buffered and let the actor arrive at the new place instead of flying to it.
    if (!aInterpolationComponent.TimePoints.empty())
    {
        constexpr float cImpossibleStep = 4096.f;
        const glm::vec3& previous = aInterpolationComponent.TimePoints.back().Position;
        const float jump = glm::distance(previous, static_cast<glm::vec3>(acPoint.Position));
        if (jump > cImpossibleStep)
        {
            aInterpolationComponent.TimePoints.clear();
            aInterpolationComponent.Position = acPoint.Position;

            // Said out loud, because the reason for this guard is a hypothesis: that the 875,458-unit
            // displacement of 2026-09-23 was two buffered points either side of a worldspace change. The guard
            // bounds the damage whatever the cause, but only a session says how often this happens, how far, and
            // from where -- and whether it lines up with somebody crossing a boundary or with nothing at all.
            static std::chrono::steady_clock::time_point s_nextLog;
            static uint32_t s_since = 0;
            ++s_since;
            const auto now = std::chrono::steady_clock::now();
            if (now >= s_nextLog)
            {
                s_nextLog = now + std::chrono::seconds(10);
                spdlog::info("JumpDiag: a buffered point was {:.0f} units from the one before it, at ({:.0f}, {:.0f}, {:.0f}); dropped the buffer so the actor arrives instead of flying. {} since the last line.",
                             jump, acPoint.Position.x, acPoint.Position.y, acPoint.Position.z, s_since);
                s_since = 0;
            }
        }
    }

    auto itor = std::begin(aInterpolationComponent.TimePoints);
    const auto end = std::cend(aInterpolationComponent.TimePoints);

    while (itor != end)
    {
        if (itor->Tick > acPoint.Tick)
        {
            aInterpolationComponent.TimePoints.insert(itor, acPoint);

            return;
        }

        ++itor;
    }

    aInterpolationComponent.TimePoints.push_back(acPoint);
}

InterpolationComponent& InterpolationSystem::Setup(World& aWorld, const entt::entity aEntity) noexcept
{
    return aWorld.emplace_or_replace<InterpolationComponent>(aEntity);
}

void InterpolationSystem::Clean(World& aWorld, const entt::entity aEntity) noexcept
{
    if (aWorld.all_of<InterpolationComponent>(aEntity))
        aWorld.remove<InterpolationComponent>(aEntity);
}
