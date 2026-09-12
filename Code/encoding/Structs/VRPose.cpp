#include <Structs/VRPose.h>

bool VRTransform::operator==(const VRTransform& acRhs) const noexcept
{
    return Position == acRhs.Position && Rotation == acRhs.Rotation;
}

bool VRTransform::operator!=(const VRTransform& acRhs) const noexcept
{
    return !this->operator==(acRhs);
}

void VRTransform::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Position.Serialize(aWriter);
    Rotation.Serialize(aWriter);
}

void VRTransform::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    Position.Deserialize(aReader);
    Rotation.Deserialize(aReader);
}

bool VRPose::operator==(const VRPose& acRhs) const noexcept
{
    if (HasData != acRhs.HasData)
        return false;

    if (!HasData)
        return true;

    return Head == acRhs.Head && LeftHand == acRhs.LeftHand && RightHand == acRhs.RightHand;
}

bool VRPose::operator!=(const VRPose& acRhs) const noexcept
{
    return !this->operator==(acRhs);
}

void VRPose::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(HasData ? 1 : 0, 1);

    if (!HasData)
        return;

    Head.Serialize(aWriter);
    LeftHand.Serialize(aWriter);
    RightHand.Serialize(aWriter);
}

void VRPose::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    uint64_t hasData = 0;
    aReader.ReadBits(hasData, 1);
    HasData = hasData != 0;

    if (!HasData)
        return;

    Head.Deserialize(aReader);
    LeftHand.Deserialize(aReader);
    RightHand.Deserialize(aReader);
}
