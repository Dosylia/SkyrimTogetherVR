#pragma once

#include <array>

#include <Structs/Quaternion_NetQuantize.h>

using TiltedPhoques::Buffer;

//! Pose of a VR player, synced alongside Movement: the upper body always, the legs when trackers drive them.
//!
//! Each bone rotation is relative to the actor's 3D root, read from the skeleton VRIK drives. The
//! receiver applies the same rotations, so no headset or controller calibration is needed.
//! HasData is false (and nothing else is written) for non-VR actors. HasLegs is true only for a player
//! whose hips and feet are driven by body trackers (SkyrimVR FBT); everyone else leaves the legs to the
//! walk animation on both sides, and the receiver needs nothing installed to show tracked legs.
struct VRPose
{
    //! Bone order is parent-before-child; see VRBodySync.cpp for the node names.
    enum Bone : uint8_t
    {
        kSpine1,
        kSpine2,
        kNeck,
        kHead,
        kLeftClavicle,
        kLeftUpperArm,
        kLeftForearm,
        kLeftHand,
        kRightClavicle,
        kRightUpperArm,
        kRightForearm,
        kRightHand,
        kUpperBoneCount,
        // Lower body, present only when HasLegs.
        kPelvis = kUpperBoneCount,
        kLeftThigh,
        kLeftCalf,
        kLeftFoot,
        kRightThigh,
        kRightCalf,
        kRightFoot,
        kBoneCount
    };

    VRPose() = default;
    ~VRPose() = default;

    bool operator==(const VRPose& acRhs) const noexcept;
    bool operator!=(const VRPose& acRhs) const noexcept;

    void Serialize(Buffer::Writer& aWriter) const noexcept;
    void Deserialize(Buffer::Reader& aReader) noexcept;

    bool HasData{false};
    bool HasLegs{false}; // the entries from kPelvis on are valid
    std::array<Quaternion_NetQuantize, kBoneCount> Bones{};

    //! Finger bones, left hand then right, five fingers of three bones each from the thumb, as rotations relative to
    //! the parent bone (the hand for the first of each finger). Sent only when they change or once a second
    //! (HasFingers); the receiver keeps the last set it got.
    static constexpr size_t kFingersPerHand = 5;
    static constexpr size_t kBonesPerFinger = 3;
    static constexpr size_t kFingerBoneCount = 2 * kFingersPerHand * kBonesPerFinger;
    bool HasFingers{false};
    std::array<Quaternion_NetQuantize, kFingerBoneCount> Fingers{};

    //! Body size: the world scale of the skeleton root ("NPC Root [Root]"), times 1000. It folds in the actor's own
    //! scale and whatever VRIK scales the body by, so a short or tall player is the same size on the other side.
    //! Sent on change and once a second (HasScale); the receiver keeps the last value.
    bool HasScale{false};
    uint16_t RootScale{1000};

    //! World position of the 3D root, sent only for a dead body that is being moved (HasRootPosition).
    //!
    //! A corpse is not driven by its character controller but by its ragdoll, so moving the reference on the
    //! receiving side leaves the visible body where its own ragdoll dropped it: the two never agreed, and nobody
    //! could drag anything (2026-09-23). The bones already survive the local animation by being written at the
    //! renderer's frame end, and the root position is written in the same place for the same reason.
    bool HasRootPosition{false};
    float RootPosition[3]{};

    //! Where the hips sit relative to the 3D root, in root space, sent while the legs are tracked (HasHips).
    //!
    //! The pose is otherwise rotations only, and that is why a hip tracker changed nothing on the other screen:
    //! crouching, leaning and hip sway move the body relative to the root without changing any rotation the
    //! protocol carried. The feet followed because the thigh and calf rotations did travel, which is the shape of
    //! the report -- feet tracked, hips not (2026-09-25). This is the missing translation. It is applied to the
    //! whole body rather than to the pelvis alone, because the spine hangs off NPC COM beside the pelvis and not
    //! below it: moving the pelvis by itself would pull the legs away from the torso.
    bool HasHips{false};
    float HipOffset[3]{};
};
