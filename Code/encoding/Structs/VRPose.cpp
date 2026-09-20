#include <Structs/VRPose.h>
#include <algorithm>

bool VRPose::operator==(const VRPose& acRhs) const noexcept
{
    if (HasData != acRhs.HasData)
        return false;

    if (!HasData)
        return true;

    if (HasLegs != acRhs.HasLegs || HasFingers != acRhs.HasFingers)
        return false;
    if (HasFingers && Fingers != acRhs.Fingers)
        return false;
    if (HasScale != acRhs.HasScale || (HasScale && RootScale != acRhs.RootScale))
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
    aWriter.WriteBits(HasFingers ? 1 : 0, 1);
    if (HasFingers)
        for (const auto& bone : Fingers)
            bone.Serialize(aWriter);
    aWriter.WriteBits(HasScale ? 1 : 0, 1);
    if (HasScale)
        aWriter.WriteBits(RootScale, 16);
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
    uint64_t hasFingers = 0;
    aReader.ReadBits(hasFingers, 1);
    HasFingers = hasFingers != 0;
    if (HasFingers)
        for (auto& bone : Fingers)
            bone.Deserialize(aReader);
    uint64_t hasScale = 0;
    aReader.ReadBits(hasScale, 1);
    HasScale = hasScale != 0;
    if (HasScale)
    {
        uint64_t scale = 0;
        aReader.ReadBits(scale, 16);
        RootScale = static_cast<uint16_t>(scale);
    }
}
