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

//! The other player's weapon (on their copy FormId, Side 0 left / 1 right) met one of this player's HIGGS bodies.
struct Clash
{
    uint32_t FormId = 0;
    uint8_t Side = 0;
    //! This player's hand that met it: 0 left, 1 right, 2 not known.
    uint8_t OwnSide = 2;
    glm::vec3 Point{};
    //! Game units a second.
    float Speed = 0.f;
    //! Whether its sound was played.
    bool Heard = false;
    //! The two started touching (felt, heard and sent); otherwise they are still touching (only weighed by the
    //! defender's rule).
    bool Start = true;
    //! How far the HIGGS body that met it was from that hand's node.
    float HandDistance = 0.f;
};
//! The next clash seen at a frame end, felt and heard as it is taken when it is a meeting; false when there is none.
bool TakeClash(Clash& aOut) noexcept;
//! A clash the other player saw on one of this player's weapons: felt on the hand holding it (0 left, 1 right).
void FeelClash(uint8_t aSide) noexcept;
//! The game's blade-block sound, at a clash's point; false when it could not be played.
bool SoundClash(const glm::vec3& acPoint) noexcept;

bool CaptureLocalPose(PlayerCharacter* apPlayer, VRPose& aOutPose) noexcept;



//! The pose of a body that is not the local player: a dead NPC this machine owns, so that the other players see it
//! lie, slump and get dragged around the way it does here (HIGGS grabs, spell pushes, a foot in the ribs).
//! Upper body, and the legs when the skeleton has them. Returns false, leaving the pose empty, when the body has not
//! moved since the last send.
bool CaptureBodyPose(Actor* apActor, VRPose& aOutPose) noexcept;
//! Watch a dead body this client does **not** own for signs of being handled here. True once per grab, when it has
//! been moving here for a third of a second: the caller then asks for the body, so that this side sends it.
//! acOwnerPosition is where the owner's updates put it: motion that follows those is the owner's, not a hand here.
bool ObserveRemoteBodyMotion(Actor* apActor, const glm::vec3& acOwnerPosition) noexcept;

//! Weapon touch: a controller pulse when your blade or your hand meets somebody else's weapon.
//!
//! Driven from the game thread, once per frame, around the loop that already walks every tracked actor:
//! BeginWeaponTouch, then ConsiderForWeaponTouch for each, then EndWeaponTouch. Works against NPCs and against
//! other players' copies alike -- the copy's weapon is a node like any other, though it is only in the right
//! place once the remote hand offset is fixed.
void BeginWeaponTouch() noexcept;
void ConsiderForWeaponTouch(Actor* apActor) noexcept;
void EndWeaponTouch() noexcept;

//! A pose without data clears the actor's pose.
void SetRemotePose(Actor* apActor, const VRPose& acPose) noexcept;
void ClearRemotePose(uint32_t aFormId) noexcept;
//! TEMPORARY: logs the magic node position against the posed hand for the first remote casts (spell offset report).
void LogCastOrigin(Actor* apActor, uint32_t aCastingSource) noexcept;

//! A cheap fingerprint of the local transforms of the first nodes below an actor's 3D root. Equal fingerprints across
//! frames on a body that is moving mean its animation graph is not advancing (the walk cycle would change them).
//! 0 when the actor has no 3D.
uint64_t SkeletonMotionFingerprint(Actor* apActor) noexcept;
//! Angle in degrees between where the headset points and a world position: 0 is dead centre, -1 when the headset
//! node cannot be read on this build. Tells whether the player is looking at something.
float HeadsetAngleTo(const NiPoint3& acPosition) noexcept;

//! TEMPORARY: one line about an actor's 3D for the invisible-copy diagnosis: root, its children, the skeleton root's
//! world scale and position.
std::string DescribeBody(Actor* apActor) noexcept;
} // namespace VRBodySync
