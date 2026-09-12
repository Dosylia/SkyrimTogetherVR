#include <TiltedOnlinePCH.h>

#include <Systems/AnimationSystem.h>

#include <Games/Animation/TESActionData.h>
#include <Games/Animation/ActorMediator.h>

#include <Games/References.h>

#include <Forms/BGSAction.h>
#include <AI/AIProcess.h>
#include <Misc/MiddleProcess.h>

#include <Messages/ClientReferencesMoveRequest.h>

#include <Components.h>
#include <World.h>

#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>

#ifdef SKYRIMVR
#include <PlayerCharacter.h>
#include <NetImmerse/NiNode.h>
#include <NetImmerse/NiTransform.h>
#endif

extern thread_local const char* g_animErrorCode;

#ifdef SKYRIMVR
namespace
{
    // Byte offsets from PlayerCharacter's own base pointer, per CommonLibVR-NG's
    // VR_NODE_DATA (RE/P/PlayerCharacter.h) - TODOVR: unverified against a live
    // game process, same caveat as NiMatrix3ToQuat.
    constexpr uint32_t kUprightHmdNodeOffset = 0x580;
    constexpr uint32_t kLeftWandNodeOffset = 0x490;
    constexpr uint32_t kRightWandNodeOffset = 0x4F8;
    constexpr uint32_t kNiAVObjectWorldTransformOffset = 0x7C;

    NiNode* GetVRTrackedNode(PlayerCharacter* apPlayer, uint32_t aOffset) noexcept
    {
        return *reinterpret_cast<NiNode**>(reinterpret_cast<uint8_t*>(apPlayer) + aOffset);
    }

    bool CaptureVRTransform(PlayerCharacter* apPlayer, uint32_t aNodeOffset, const NiPoint3& acActorPosition, VRTransform& aOutTransform) noexcept
    {
        NiNode* pNode = GetVRTrackedNode(apPlayer, aNodeOffset);
        if (!pNode)
            return false;

        const auto& worldTransform = *reinterpret_cast<const NiTransform*>(reinterpret_cast<const uint8_t*>(pNode) + kNiAVObjectWorldTransformOffset);

        aOutTransform.Position = static_cast<glm::vec3>(worldTransform.translate) - static_cast<glm::vec3>(acActorPosition);
        aOutTransform.Rotation = NiMatrix3ToQuat(worldTransform.rotate);
        return true;
    }
}
#endif

void AnimationSystem::Update(World& aWorld, Actor* apActor, RemoteAnimationComponent& aAnimationComponent, const uint64_t aTick) noexcept
{
    auto& actions = aAnimationComponent.TimePoints;

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
        const bool hasHead = CaptureVRTransform(pPlayer, kUprightHmdNodeOffset, pActor->position, pose.Head);
        const bool hasLeftHand = CaptureVRTransform(pPlayer, kLeftWandNodeOffset, pActor->position, pose.LeftHand);
        const bool hasRightHand = CaptureVRTransform(pPlayer, kRightWandNodeOffset, pActor->position, pose.RightHand);

        pose.HasData = hasHead && hasLeftHand && hasRightHand;
        update.UpdatedVRPose = pose;
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
