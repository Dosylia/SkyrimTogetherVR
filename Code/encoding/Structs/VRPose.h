#pragma once

#include <Structs/Vector3_NetQuantize.h>
#include <Structs/Quaternion_NetQuantize.h>

using TiltedPhoques::Buffer;

//! A single tracked VR point (head or hand), position relative to the actor's
//! own root so it doesn't drift out of sync with Movement's own position field.
struct VRTransform
{
    VRTransform() = default;
    ~VRTransform() = default;

    bool operator==(const VRTransform& acRhs) const noexcept;
    bool operator!=(const VRTransform& acRhs) const noexcept;

    void Serialize(Buffer::Writer& aWriter) const noexcept;
    void Deserialize(Buffer::Reader& aReader) noexcept;

    Vector3_NetQuantize Position{};
    Quaternion_NetQuantize Rotation{};
};

//! Head + hand controller pose, synced alongside Movement for VR players.
//! HasData is false (and nothing else is written) for non-VR actors.
struct VRPose
{
    VRPose() = default;
    ~VRPose() = default;

    bool operator==(const VRPose& acRhs) const noexcept;
    bool operator!=(const VRPose& acRhs) const noexcept;

    void Serialize(Buffer::Writer& aWriter) const noexcept;
    void Deserialize(Buffer::Reader& aReader) noexcept;

    bool HasData{false};
    VRTransform Head{};
    VRTransform LeftHand{};
    VRTransform RightHand{};
};
