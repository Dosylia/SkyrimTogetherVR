#include <Structs/VRPose.h>

bool VRPose::operator==(const VRPose& acRhs) const noexcept
{
    if (HasData != acRhs.HasData)
        return false;

    if (!HasData)
        return true;

    return Bones == acRhs.Bones;
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

    for (const auto& bone : Bones)
        bone.Serialize(aWriter);
}

void VRPose::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    uint64_t hasData = 0;
    aReader.ReadBits(hasData, 1);
    HasData = hasData != 0;

    if (!HasData)
        return;

    for (auto& bone : Bones)
        bone.Deserialize(aReader);
}
