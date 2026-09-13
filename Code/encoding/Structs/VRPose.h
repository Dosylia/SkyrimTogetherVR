#pragma once

#include <array>

#include <Structs/Quaternion_NetQuantize.h>

using TiltedPhoques::Buffer;

//! Upper-body pose of a VR player, synced alongside Movement.
//!
//! Each entry is the rotation of one skeleton bone relative to the actor's 3D root node
//! (root.world.rotate^-1 * bone.world.rotate), captured from the local player's
//! third-person skeleton - which VRIK drives from the headset and controllers. The
//! receiving client applies the same root-relative rotations to the remote player's
//! skeleton, so no IK or HMD/controller axis calibration is needed.
//!
//! HasData is false (and nothing else is written) for non-VR actors.
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
        kBoneCount
    };

    VRPose() = default;
    ~VRPose() = default;

    bool operator==(const VRPose& acRhs) const noexcept;
    bool operator!=(const VRPose& acRhs) const noexcept;

    void Serialize(Buffer::Writer& aWriter) const noexcept;
    void Deserialize(Buffer::Reader& aReader) noexcept;

    bool HasData{false};
    std::array<Quaternion_NetQuantize, kBoneCount> Bones{};
};
