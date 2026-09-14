#include <TiltedOnlinePCH.h>

#include <Systems/InterpolationSystem.h>
#include <Components.h>

#include <AI/AIProcess.h>
#include <Misc/MiddleProcess.h>

#include <Games/References.h>
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
            for (size_t i = 0; i < VRPose::kBoneCount; ++i)
                vrPose.Bones[i] = glm::slerp(static_cast<glm::quat>(pBefore->VRPoseData.Bones[i]), static_cast<glm::quat>(pAfter->VRPoseData.Bones[i]), poseDelta);
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

    apActor->ForcePosition(position);
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
