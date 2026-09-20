#pragma once

#include <Structs/VRPose.h>

struct Actor;
struct PlayerCharacter;

//! Syncs the pose of VR players: spine, head, arms and hands always; hips, thighs, calves and feet when the
//! sender runs a body-tracking plugin (SkyrimVR FBT). The receiver needs nothing installed for either.
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

//! A cheap fingerprint of the local transforms of the first nodes below an actor's 3D root. Equal fingerprints across
//! frames on a body that is moving mean its animation graph is not advancing (the walk cycle would change them).
//! 0 when the actor has no 3D.
uint64_t SkeletonMotionFingerprint(Actor* apActor) noexcept;
//! TEMPORARY: one line about an actor's 3D for the invisible-copy diagnosis: root, its children, the skeleton root's
//! world scale and position.
std::string DescribeBody(Actor* apActor) noexcept;
} // namespace VRBodySync
