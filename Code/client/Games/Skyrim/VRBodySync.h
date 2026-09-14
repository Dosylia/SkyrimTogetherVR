#pragma once

#include <Structs/VRPose.h>

struct Actor;
struct PlayerCharacter;

//! Syncs the upper-body pose of VR players (spine, head, arms, hands).
//!
//! Local side: reads the root-relative bone rotations of the player's skeleton (driven by VRIK from
//! the headset and controllers). Remote side: once per frame, at the renderer's frame end, the remote
//! skeleton is posed from the latest interpolated pose. Writing from the animation job threads instead
//! let the renderer read half-written bones: the remote body flickered, and spells and the face were
//! left on the animation pose.
namespace VRBodySync
{
//! Renderer frame end (BSGraphics StopTimer). One thread, once per frame.
void OnFrameEnd() noexcept;

bool CaptureLocalPose(PlayerCharacter* apPlayer, VRPose& aOutPose) noexcept;

//! A pose without data clears the actor's pose.
void SetRemotePose(Actor* apActor, const VRPose& acPose) noexcept;
void ClearRemotePose(uint32_t aFormId) noexcept;
//! TEMPORARY: logs the magic node position against the posed hand for the first remote casts (spell offset report).
void LogCastOrigin(Actor* apActor, uint32_t aCastingSource) noexcept;
} // namespace VRBodySync
