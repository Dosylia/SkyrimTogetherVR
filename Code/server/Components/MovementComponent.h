#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <Structs/AnimationVariables.h>
#include <Structs/VRPose.h>

struct MovementComponent
{
    uint64_t Tick;
    glm::vec3 Position;
    glm::vec3 Rotation;
    AnimationVariables Variables;
    float Direction;
    bool Flying{}; // a dragon in the air in its owner's game (Movement::Flying)
    VRPose VRPoseData; // VR players' upper-body pose, relayed as-is to other players

    bool Sent;
};
