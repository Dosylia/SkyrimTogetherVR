#pragma once

#include <Structs/VRPose.h>

struct Actor;
struct PlayerCharacter;

//! Syncs the upper-body pose of VR players (spine, head, arms, hands).
//!
//! Local side: reads the root-relative bone rotations of the player's skeleton (driven by VRIK from
//! the headset and controllers). Remote side: after the game animates a remote actor, those bones
//! are overwritten with the latest interpolated pose.
namespace VRBodySync
{
bool CaptureLocalPose(PlayerCharacter* apPlayer, VRPose& aOutPose) noexcept;

//! A pose without data clears the actor's pose.
void SetRemotePose(Actor* apActor, const VRPose& acPose) noexcept;
void ClearRemotePose(uint32_t aFormId) noexcept;
//! TEMPORARY: logs the magic node position against the posed hand for the first remote casts (spell offset report).
void LogCastOrigin(Actor* apActor, uint32_t aCastingSource) noexcept;
} // namespace VRBodySync
