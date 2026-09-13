#pragma once

#include <Structs/VRPose.h>

struct Actor;
struct PlayerCharacter;

//! Syncs the upper-body pose of VR players (head, spine, arms, hands).
//!
//! Local side: read the root-relative rotations of the player's third-person skeleton bones
//! (which VRIK drives from the headset and controllers) into a VRPose.
//! Remote side: keep the latest interpolated pose per remote actor and, right after the game
//! animates that actor (Actor::UpdateAnimation, vtable slot 0x7D), overwrite the same bones.
namespace VRBodySync
{
bool CaptureLocalPose(PlayerCharacter* apPlayer, VRPose& aOutPose) noexcept;

//! Called from the main thread with the interpolated pose; a pose without data clears it.
void SetRemotePose(Actor* apActor, const VRPose& acPose) noexcept;
void ClearRemotePose(uint32_t aFormId) noexcept;
} // namespace VRBodySync
