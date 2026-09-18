#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <Structs/AnimationVariables.h>
#include <Structs/VRPose.h>

struct InterpolationComponent
{
    struct TimePoint
    {
        uint64_t Tick{};
        glm::vec3 Position{};
        glm::vec3 Rotation{};
        AnimationVariables Variables{};
        float Direction{};
        VRPose VRPoseData{};

        TimePoint() = default;
        TimePoint(const TimePoint&) = default;
        TimePoint& operator=(const TimePoint&) = default;
    };

    List<TimePoint> TimePoints;
    glm::vec3 Position;
    // Latest interpolated VR pose, applied to the remote actor by VRBodySync.
    VRPose InterpolatedVRPose;
    // See RemoteAnimationComponent::ForeignGraph: the owner's animation variables are not applied either.
    bool ForeignGraph{false};
};
