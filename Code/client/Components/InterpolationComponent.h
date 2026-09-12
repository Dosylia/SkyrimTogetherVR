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
    // Latest interpolated VR pose for this remote actor, in world space (VRPose's
    // own Head/LeftHand/RightHand positions are actor-relative; this is that plus
    // Position). Not yet consumed by anything - rendering remote VR avatars is a
    // separate follow-up.
    VRPose InterpolatedVRPose;
};
