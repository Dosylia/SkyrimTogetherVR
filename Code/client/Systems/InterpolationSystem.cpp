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

    delta = TiltedPhoques::Min(delta, 1.0f);

    const NiPoint3 position{TiltedPhoques::Lerp(first.Position, second.Position, delta)};

    aInterpolationComponent.Position = position;

    // The VR pose is played back with a shorter delay (aPoseTick) than movement: hands 300 ms behind felt
    // out of sync.
    auto& vrPose = aInterpolationComponent.InterpolatedVRPose;
    vrPose.HasData = false;
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
            // Legs only between two points that both carry them (the sender's trackers can come and go).
            vrPose.HasLegs = pBefore->VRPoseData.HasLegs && pAfter->VRPoseData.HasLegs;
            const size_t boneCount = vrPose.HasLegs ? VRPose::kBoneCount : VRPose::kUpperBoneCount;
            for (size_t i = 0; i < boneCount; ++i)
                vrPose.Bones[i] = glm::slerp(static_cast<glm::quat>(pBefore->VRPoseData.Bones[i]), static_cast<glm::quat>(pAfter->VRPoseData.Bones[i]), poseDelta);
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
        }
    }

#ifdef SKYRIMVR
    VRBodySync::SetRemotePose(apActor, vrPose);
#endif

    if (!apActor)
        return;

    // A dying body falls with its own ragdoll. Once dead it is moved to where the owner's corpse lies, but only
    // when it is clearly elsewhere, so a settled ragdoll isn't pulled around every frame.
    if (apActor->actorState.IsDying())
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

        if (const auto it = s_lastPlacedZ.find(apActor->formID); it != s_lastPlacedZ.end())
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
                spdlog::info("SinkDiag: {} placements found a remote actor lower than where it was last put, worst {:.0f} units on {:X}", s_sunkPlacements, s_worstSink, s_worstActor);
            s_sunkPlacements = 0;
            s_worstSink = 0.f;
            s_worstActor = 0;
        }
    }
#endif

#ifdef SKYRIMVR
    // TEMPORARY: remote NPCs slide for the receiving player while the owner sees them walk (2026-09-19). Either their
    // animation graph is not advancing at all (Actor::Process is skipped for remote actors, and on VR that id may
    // cover more than AI) or it advances with wrong variables. Every fourth frame, for a remote NPC that moved
    // since its last sample, the skeleton's local transforms are fingerprinted: a body that moves while its bones
    // never change has a frozen graph. Remove once understood.
    if (!apActor->GetExtension()->IsRemotePlayer() && !apActor->actorState.IsDeadOrDying())
    {
        struct MotionSample
        {
            uint64_t Fingerprint = 0;
            glm::vec3 Position{};
            uint32_t Frame = 0;
            uint32_t FrozenSamples = 0;
        };
        static TiltedPhoques::Map<uint32_t, MotionSample> s_samples;
        static uint32_t s_frame = 0;
        static uint32_t s_moving = 0;
        static uint32_t s_frozen = 0;
        static uint32_t s_worstActor = 0;
        static uint32_t s_worstFrozen = 0;
        static std::chrono::steady_clock::time_point s_nextLog = std::chrono::steady_clock::now() + 10s;

        ++s_frame;
        MotionSample& sample = s_samples[apActor->formID];
        if (s_frame - sample.Frame >= 4)
        {
            sample.Frame = s_frame;
            const bool moved = glm::distance(position, sample.Position) > 2.f;
            sample.Position = position;
            if (moved)
            {
                const uint64_t fingerprint = VRBodySync::SkeletonMotionFingerprint(apActor);
                if (fingerprint != 0 && fingerprint == sample.Fingerprint)
                    ++sample.FrozenSamples;
                else
                    sample.FrozenSamples = 0;
                sample.Fingerprint = fingerprint;

                ++s_moving;
                if (sample.FrozenSamples >= 5)
                {
                    ++s_frozen;
                    if (sample.FrozenSamples > s_worstFrozen)
                    {
                        s_worstFrozen = sample.FrozenSamples;
                        s_worstActor = apActor->formID;
                    }
                }
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= s_nextLog)
        {
            s_nextLog = now + 10s;
            if (s_moving)
            {
                const Actor* pWorst = s_worstActor ? Cast<Actor>(TESForm::GetById(s_worstActor)) : nullptr;
                const TESNPC* pBase = pWorst ? Cast<TESNPC>(pWorst->baseForm) : nullptr;
                spdlog::info("MotionDiag: {} samples of remote NPCs moving, {} with bones that had not changed for 5+ samples; worst {:X} ({}) frozen for {} samples", s_moving, s_frozen,
                             s_worstActor, pBase ? pBase->fullName.value.AsAscii() : "-", s_worstFrozen);
            }
            s_moving = 0;
            s_frozen = 0;
            s_worstActor = 0;
            s_worstFrozen = 0;
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
