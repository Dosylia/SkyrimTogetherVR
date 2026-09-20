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
};
