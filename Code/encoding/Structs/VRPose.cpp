#include <Structs/VRPose.h>
#include <algorithm>

bool VRPose::operator==(const VRPose& acRhs) const noexcept
{
    if (HasData != acRhs.HasData)
        return false;

    if (!HasData)
        return true;

    if (HasLegs != acRhs.HasLegs)
        return false;
    const size_t count = HasLegs ? kBoneCount : kUpperBoneCount;
    return std::equal(Bones.begin(), Bones.begin() + count, acRhs.Bones.begin());
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

    aWriter.WriteBits(HasLegs ? 1 : 0, 1);
    const size_t count = HasLegs ? kBoneCount : kUpperBoneCount;
    for (size_t i = 0; i < count; ++i)
        Bones[i].Serialize(aWriter);
}

void VRPose::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    uint64_t hasData = 0;
    aReader.ReadBits(hasData, 1);
    HasData = hasData != 0;

    if (!HasData)
        return;

    uint64_t hasLegs = 0;
    aReader.ReadBits(hasLegs, 1);
    HasLegs = hasLegs != 0;
    const size_t count = HasLegs ? kBoneCount : kUpperBoneCount;
    for (size_t i = 0; i < count; ++i)
        Bones[i].Deserialize(aReader);
}
