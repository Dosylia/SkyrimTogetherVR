#include <TiltedOnlinePCH.h>

#include <Systems/AnimationSystem.h>

#include <Games/Animation/TESActionData.h>
#include <Games/Animation/ActorMediator.h>

#include <Games/References.h>

#include <Forms/BGSAction.h>
#include <Forms/TESNPC.h>
#include <AI/AIProcess.h>
#include <Misc/MiddleProcess.h>

#include <Messages/ClientReferencesMoveRequest.h>

#include <Components.h>
#include <World.h>

#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>

#ifdef SKYRIMVR
#include <PlayerCharacter.h>
#include <Games/Skyrim/VRBodySync.h>
#endif

extern thread_local const char* g_animErrorCode;

void AnimationSystem::Update(World& aWorld, Actor* apActor, RemoteAnimationComponent& aAnimationComponent, const uint64_t aTick) noexcept
{
    auto& actions = aAnimationComponent.TimePoints;

    if (aAnimationComponent.ForeignGraph)
    {
        // The owner's actions belong to another creature's graph (see MarkForeignGraph); nothing here fits.
        actions.clear();
        return;
    }

    const auto it = std::begin(actions);
    if (it != std::end(actions) && it->Tick <= aTick)
    {
        // Check if animation graph is ready before attempting to play animations
        if (!apActor->animationGraphHolder.IsReady())
        {
            // Animation graph not ready, keep the action in queue and try again later
            return;
        }
        if (aAnimationComponent.ReplayCount > 0 && aAnimationComponent.ResetAnimationGraphForReplay)
        {
            apActor->animationGraphHolder.RevertAnimationGraphManager();
            aAnimationComponent.ResetAnimationGraphForReplay = false;
        }

        const auto& first = *it;

        const auto actionId = first.ActionId;
        const auto targetId = first.TargetId;

        const auto pAction = Cast<BGSAction>(TESForm::GetById(actionId));
        const auto pTarget = Cast<TESObjectREFR>(TESForm::GetById(targetId));

        // The owner's state word also carries the life state (bits 21-24). Actions are replayed 300 ms late, so the
        // ones arriving just after a body died here were recorded while it was still alive, and copying the whole
        // word stood the corpse back up as a living enemy. Once dying or dead, keep that part (death messages own it).
        constexpr uint32_t cLifeStateMask = 0x1E00000;
        if (apActor->actorState.IsDeadOrDying())
            apActor->actorState.flags1 = (first.State1 & ~cLifeStateMask) | (apActor->actorState.flags1 & cLifeStateMask);
        else
            apActor->actorState.flags1 = first.State1;
        apActor->actorState.flags2 = first.State2;

        apActor->LoadAnimationVariables(first.Variables);

        aAnimationComponent.LastRanAction = first;

        // Play the animation
        TESActionData actionData(first.Type & 0x3, apActor, pAction, pTarget);
        actionData.eventName = BSFixedString(first.EventName.c_str());
        actionData.idleForm = Cast<TESIdleForm>(TESForm::GetById(first.IdleId));
        actionData.someFlag = ((first.Type & 0x4) != 0) ? 1 : 0;

        const auto result = ActorMediator::Get()->ForceAction(&actionData);

        // TEMPORARY: remote NPCs slide instead of walking on the receiving side, and AnimDiag shows the game refusing
        // most of their moveStart and turnStop replays (2026-09-19, 7 of 9 moveStart refused). This names, for one
        // refused replay per 10 s, everything the refusal could depend on. Remove once understood.
        if (!result)
        {
            static std::chrono::steady_clock::time_point s_nextSample{};
            const auto now = std::chrono::steady_clock::now();
            if (now >= s_nextSample)
            {
                s_nextSample = now + 10s;
                const auto* pBase = Cast<TESNPC>(apActor->baseForm);
                spdlog::info("ReplayDiag: refused '{}' on {:X} ({}): action {:X} resolved {}, idle {:X} resolved {}, type {}, target {:X} resolved {}, graph ready {}, has 3D {}, "
                             "has process {}, flags1 {:#x}, flags2 {:#x}, dead or dying {}, replay left {}, queued {}",
                             first.EventName.c_str(), apActor->formID, pBase ? pBase->fullName.value.AsAscii() : "?", actionId, pAction != nullptr, first.IdleId,
                             actionData.idleForm != nullptr, first.Type, targetId, pTarget != nullptr, apActor->animationGraphHolder.IsReady(),
                             apActor->GetNiNode() != nullptr, apActor->currentProcess != nullptr, apActor->actorState.flags1, apActor->actorState.flags2,
                             apActor->actorState.IsDeadOrDying(), aAnimationComponent.ReplayCount, actions.size());
            }
        }

        // TEMPORARY sliding diagnostic: actors were reported sliding instead of walking. Counts replayed actions and
        // the ones the game refused, per event name, and logs a summary every 10 s. Remove once understood.
        //
        // Read the failure counts with care. ForceAction applies the animation variables whether or not it returns
        // true, and the events that carry direction rather than state (moveForward, moveBackward) are not ones the
        // behaviour graph accepts as actions at all, so they always count as failed and always did. A single actor
        // being respawned in a loop also floods these totals, which is what happened the session this line was read
        // from, so the busiest actors are named: one form id with a count far above the rest is a respawn loop rather
        // than an animation problem.
        {
            struct EventStats
            {
                uint32_t Replayed = 0;
                uint32_t Failed = 0;
            };
            static TiltedPhoques::Map<TiltedPhoques::String, EventStats> s_players;
            static TiltedPhoques::Map<TiltedPhoques::String, EventStats> s_npcs;
            static TiltedPhoques::Map<uint32_t, uint32_t> s_byActor;
            static std::chrono::steady_clock::time_point s_nextLog = std::chrono::steady_clock::now() + 10s;

            auto& stats = (apActor->GetExtension()->IsPlayer() ? s_players : s_npcs)[first.EventName.empty() ? "(no event)" : first.EventName.c_str()];
            ++stats.Replayed;
            if (!result)
                ++stats.Failed;
            ++s_byActor[apActor->formID];

            const auto now = std::chrono::steady_clock::now();
            if (now >= s_nextLog)
            {
                s_nextLog = now + 10s;
                for (auto* pStats : {&s_players, &s_npcs})
                {
                    if (pStats->empty())
                        continue;
                    std::string line;
                    for (const auto& [name, eventStats] : *pStats)
                        line += fmt::format("{} {}/{} failed, ", name.c_str(), eventStats.Failed, eventStats.Replayed);
                    spdlog::info("AnimDiag {}: {}", pStats == &s_players ? "remote players" : "remote NPCs", line);
                    pStats->clear();
                }

                if (!s_byActor.empty())
                {
                    TiltedPhoques::Vector<std::pair<uint32_t, uint32_t>> busiest(s_byActor.begin(), s_byActor.end());
                    std::partial_sort(busiest.begin(), busiest.begin() + std::min<size_t>(5, busiest.size()), busiest.end(),
                                      [](const auto& acLhs, const auto& acRhs) { return acLhs.second > acRhs.second; });

                    std::string line;
                    for (size_t i = 0; i < busiest.size() && i < 5; ++i)
                        line += fmt::format("{:X} x{}, ", busiest[i].first, busiest[i].second);

                    spdlog::info("AnimDiag busiest actors ({} in total): {}", s_byActor.size(), line);
                    s_byActor.clear();
                }
            }
        }

        if (aAnimationComponent.ReplayCount > 0)
            aAnimationComponent.ReplayCount--;

        actions.pop_front();
    }
}

void AnimationSystem::Setup(World& aWorld, const entt::entity aEntity) noexcept
{
    aWorld.emplace_or_replace<RemoteAnimationComponent>(aEntity);
}

void AnimationSystem::Clean(World& aWorld, const entt::entity aEntity) noexcept
{
    if (aWorld.all_of<RemoteAnimationComponent>(aEntity))
        aWorld.remove<RemoteAnimationComponent>(aEntity);
}

void AnimationSystem::AddActionsForReplay(RemoteAnimationComponent& aAnimationComponent,
                                          const ActionReplayChain& acReplay) noexcept
{
    aAnimationComponent.TimePoints.insert(aAnimationComponent.TimePoints.end(), acReplay.Actions.begin(),
                                          acReplay.Actions.end());
    aAnimationComponent.ReplayCount = acReplay.Actions.size();
    aAnimationComponent.ResetAnimationGraphForReplay = acReplay.ResetAnimationGraph;
}

void AnimationSystem::AddAction(RemoteAnimationComponent& aAnimationComponent, const std::string& acActionDiff) noexcept
{
    auto itor = std::begin(aAnimationComponent.TimePoints);
    const auto end = std::cend(aAnimationComponent.TimePoints);

    auto& lastProcessedAction = aAnimationComponent.LastProcessedAction;

    TiltedPhoques::ViewBuffer buffer((uint8_t*)acActionDiff.data(), acActionDiff.size());
    Buffer::Reader reader(&buffer);

    lastProcessedAction.ApplyDifferential(reader);

    aAnimationComponent.TimePoints.push_back(lastProcessedAction);

    // Update plays at most one action per frame, and nothing bounded this queue. An actor whose owner is stuck in
    // a loop therefore builds a backlog that never drains: on 2026-09-24 a single Redoran Guard reached 717
    // refused 'Unequip' replays while the rest of the world starved for updates. An action this far behind is not
    // worth playing anyway, so the oldest are dropped and the actor catches up with the present.
    constexpr size_t cMaxQueued = 96; // over a second of backlog at the frame rate these replay at
    if (aAnimationComponent.TimePoints.size() > cMaxQueued)
    {
        const size_t dropped = aAnimationComponent.TimePoints.size() - cMaxQueued;
        for (size_t i = 0; i < dropped; ++i)
            aAnimationComponent.TimePoints.pop_front();
        if (aAnimationComponent.ReplayCount > dropped)
            aAnimationComponent.ReplayCount -= dropped;
        else
            aAnimationComponent.ReplayCount = 0;

        static std::chrono::steady_clock::time_point s_nextSaid{};
        const auto now = std::chrono::steady_clock::now();
        if (now >= s_nextSaid)
        {
            s_nextSaid = now + 10s;
            spdlog::warn("Animation replay backlog over {} actions, dropped {} stale ones; an owner is looping an action", cMaxQueued, dropped);
        }
    }
}

void AnimationSystem::Serialize(World& aWorld, ClientReferencesMoveRequest& aMovementSnapshot, LocalComponent& localComponent, LocalAnimationComponent& animationComponent, FormIdComponent& formIdComponent)
{
    const auto pForm = TESForm::GetById(formIdComponent.Id);
    const auto pActor = Cast<Actor>(pForm);
    if (!pActor)
        return;

    auto& update = aMovementSnapshot.Updates[localComponent.Id];
    auto& movement = update.UpdatedMovement;

    if (const auto pCell = pActor->parentCell)
        World::Get().GetModSystem().GetServerModId(pCell->formID, movement.CellId.ModId, movement.CellId.BaseId);

    if (const auto pWorldSpace = pActor->GetWorldSpace())
        World::Get().GetModSystem().GetServerModId(pWorldSpace->formID, movement.WorldSpaceId.ModId, movement.WorldSpaceId.BaseId);

    movement.Position = pActor->position;

    movement.Rotation.x = pActor->rotation.x;
    movement.Rotation.y = pActor->rotation.z;

    pActor->SaveAnimationVariables(movement.Variables);

    if (pActor->currentProcess && pActor->currentProcess->middleProcess)
    {
        movement.Direction = pActor->currentProcess->middleProcess->direction;
    }

#ifdef SKYRIMVR
    if (pActor == PlayerCharacter::Get())
    {
        auto* pPlayer = static_cast<PlayerCharacter*>(pActor);

        VRPose pose{};
        if (!VRBodySync::CaptureLocalPose(pPlayer, pose))
        {
            // The other player then sees this body in the plain animation pose (sword held up in the vanilla idle,
            // 2026-09-18 screenshots). Nothing logged that before, so it could not be told apart from a receive
            // side problem.
            static std::chrono::steady_clock::time_point s_nextWarn{};
            const auto now = std::chrono::steady_clock::now();
            if (now >= s_nextWarn)
            {
                s_nextWarn = now + 10s;
                spdlog::warn("VRBodySync: could not read the local VR pose (skeleton bones not found), the other player sees the animation pose instead");
            }
        }
        update.UpdatedVRPose = pose;
    }
    else if (pActor->actorState.IsDead())
    {
        // A body this machine owns, within reach of this player. Sending its bones is what lets the other players
        // see it being dragged, thrown or shoved here instead of seeing it snap between two resting places. It only
        // goes on the wire while the body is actually moving (CaptureBodyPose returns false otherwise), so a field
        // of corpses left alone costs nothing, and the far ones are never read at all.
        constexpr float cReachSquared = 600.f * 600.f;
        const auto* pPlayer = PlayerCharacter::Get();
        if (pPlayer)
        {
            const glm::vec3 toPlayer = static_cast<glm::vec3>(pActor->position) - static_cast<glm::vec3>(pPlayer->position);
            if (glm::dot(toPlayer, toPlayer) <= cReachSquared)
            {
                VRPose pose{};
                if (VRBodySync::CaptureBodyPose(pActor, pose))
                    update.UpdatedVRPose = pose;
            }
        }
    }
#endif

    for (auto& entry : animationComponent.Actions)
    {
        update.ActionEvents.push_back(entry);
    }

    auto latestAction = animationComponent.GetLatestAction();

    if (latestAction)
        localComponent.CurrentAction = latestAction.MoveResult();

    animationComponent.Actions.clear();
}

bool AnimationSystem::Serialize(World& aWorld, const ActionEvent& aActionEvent, const ActionEvent& aLastProcessedAction, std::string* apData)
{
    uint32_t actionBaseId = 0;
    uint32_t actionModId = 0;
    if (!aWorld.GetModSystem().GetServerModId(aActionEvent.ActionId, actionModId, actionBaseId))
        return false;

    uint32_t targetBaseId = 0;
    uint32_t targetModId = 0;
    if (!aWorld.GetModSystem().GetServerModId(aActionEvent.TargetId, targetModId, targetBaseId))
        return false;

    uint8_t scratch[1 << 14];
    TiltedPhoques::ViewBuffer buffer(scratch, std::size(scratch));
    Buffer::Writer writer(&buffer);
    aActionEvent.GenerateDifferential(aLastProcessedAction, writer);

    apData->assign(buffer.GetData(), buffer.GetData() + writer.Size());

    return true;
}
