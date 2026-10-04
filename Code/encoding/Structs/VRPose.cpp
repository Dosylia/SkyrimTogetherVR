#include <Structs/VRPose.h>
#include <algorithm>
#include <cstring>

bool VRPose::operator==(const VRPose& acRhs) const noexcept
{
    if (HasData != acRhs.HasData)
        return false;

    if (!HasData)
        return true;

    if (NoBones != acRhs.NoBones)
        return false;
    if (HasFingers != acRhs.HasFingers)
        return false;
    if (HasFingers && Fingers != acRhs.Fingers)
        return false;
    if (HasScale != acRhs.HasScale || (HasScale && RootScale != acRhs.RootScale))
        return false;
    if (HasRootPosition != acRhs.HasRootPosition)
        return false;
    if (HasHips != acRhs.HasHips)
        return false;
    if (HasHips && !std::equal(std::begin(HipOffset), std::end(HipOffset), std::begin(acRhs.HipOffset)))
        return false;
    if (HasHandCheck != acRhs.HasHandCheck)
        return false;
    if (HasHandCheck && (!std::equal(std::begin(LeftHandOffset), std::end(LeftHandOffset), std::begin(acRhs.LeftHandOffset)) ||
                         !std::equal(std::begin(RightHandOffset), std::end(RightHandOffset), std::begin(acRhs.RightHandOffset))))
        return false;
    if (HasRootPosition && !std::equal(std::begin(RootPosition), std::end(RootPosition), std::begin(acRhs.RootPosition)))
        return false;
    if (HasWeapons != acRhs.HasWeapons)
        return false;
    if (HasWeapons)
    {
        for (size_t side = 0; side < 2; ++side)
        {
            if (WeaponHeld[side] != acRhs.WeaponHeld[side])
                return false;
            if (WeaponHeld[side] && (WeaponRotation[side] != acRhs.WeaponRotation[side] ||
                                     !std::equal(std::begin(WeaponOffset[side]), std::end(WeaponOffset[side]), std::begin(acRhs.WeaponOffset[side]))))
                return false;
        }
    }
    if (NoBones)
        return true;

    // Compared here rather than with the other flags above, because a boneless pose does not put this on the wire
    // at all -- the serialiser skips it and the receiver reads back false. Comparing it unconditionally meant a
    // boneless pose could never equal the pose rebuilt from it, so it would look changed on every frame and be
    // re-sent for ever while standing still.
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

    aWriter.WriteBits(NoBones ? 1 : 0, 1);
    if (!NoBones)
    {
        aWriter.WriteBits(HasLegs ? 1 : 0, 1);
        const size_t count = HasLegs ? kBoneCount : kUpperBoneCount;
        for (size_t i = 0; i < count; ++i)
            Bones[i].Serialize(aWriter);
    }
    aWriter.WriteBits(HasFingers ? 1 : 0, 1);
    if (HasFingers)
        for (const auto& bone : Fingers)
            bone.Serialize(aWriter);
    aWriter.WriteBits(HasScale ? 1 : 0, 1);
    if (HasScale)
        aWriter.WriteBits(RootScale, 16);
    aWriter.WriteBits(HasRootPosition ? 1 : 0, 1);
    if (HasRootPosition)
    {
        // Raw bits, as Quaternion_NetQuantize does: world coordinates run to six figures, so quantising them
        // would move the body further than the drag being sent.
        for (const float value : RootPosition)
        {
            uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            aWriter.WriteBits(bits, 32);
        }
    }
    aWriter.WriteBits(HasHandCheck ? 1 : 0, 1);
    if (HasHandCheck)
    {
        for (const float value : LeftHandOffset)
        {
            uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            aWriter.WriteBits(bits, 32);
        }
        for (const float value : RightHandOffset)
        {
            uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            aWriter.WriteBits(bits, 32);
        }
    }
    aWriter.WriteBits(HasHips ? 1 : 0, 1);
    if (HasHips)
    {
        // Root-relative, so a few hundred units at most, but written raw like the root position above: the body
        // lands where this says, and a quantisation step here is a step the hips would visibly sit wrong by.
        for (const float value : HipOffset)
        {
            uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            aWriter.WriteBits(bits, 32);
        }
    }
    aWriter.WriteBits(HasWeapons ? 1 : 0, 1);
    if (HasWeapons)
    {
        for (size_t side = 0; side < 2; ++side)
        {
            aWriter.WriteBits(WeaponHeld[side] ? 1 : 0, 1);
            if (!WeaponHeld[side])
                continue;
            WeaponRotation[side].Serialize(aWriter);
            // Raw, like the hips: a few units from the hand, and a step here is a step the blade sits off by.
            for (const float value : WeaponOffset[side])
            {
                uint32_t bits = 0;
                std::memcpy(&bits, &value, sizeof(bits));
                aWriter.WriteBits(bits, 32);
            }
        }
    }
}

void VRPose::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    uint64_t hasData = 0;
    aReader.ReadBits(hasData, 1);
    HasData = hasData != 0;

    if (!HasData)
        return;

    uint64_t noBones = 0;
    aReader.ReadBits(noBones, 1);
    NoBones = noBones != 0;
    HasLegs = false;
    if (!NoBones)
    {
        uint64_t hasLegs = 0;
        aReader.ReadBits(hasLegs, 1);
        HasLegs = hasLegs != 0;
        const size_t count = HasLegs ? kBoneCount : kUpperBoneCount;
        for (size_t i = 0; i < count; ++i)
            Bones[i].Deserialize(aReader);
    }
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
    uint64_t hasRootPosition = 0;
    aReader.ReadBits(hasRootPosition, 1);
    HasRootPosition = hasRootPosition != 0;
    if (HasRootPosition)
    {
        for (float& value : RootPosition)
        {
            uint64_t bits = 0;
            aReader.ReadBits(bits, 32);
            const uint32_t raw = static_cast<uint32_t>(bits);
            std::memcpy(&value, &raw, sizeof(value));
        }
    }
    uint64_t hasHandCheck = 0;
    aReader.ReadBits(hasHandCheck, 1);
    HasHandCheck = hasHandCheck != 0;
    if (HasHandCheck)
    {
        for (float& value : LeftHandOffset)
        {
            uint64_t bits = 0;
            aReader.ReadBits(bits, 32);
            const uint32_t raw = static_cast<uint32_t>(bits);
            std::memcpy(&value, &raw, sizeof(value));
        }
        for (float& value : RightHandOffset)
        {
            uint64_t bits = 0;
            aReader.ReadBits(bits, 32);
            const uint32_t raw = static_cast<uint32_t>(bits);
            std::memcpy(&value, &raw, sizeof(value));
        }
    }
    uint64_t hasHips = 0;
    aReader.ReadBits(hasHips, 1);
    HasHips = hasHips != 0;
    if (HasHips)
    {
        for (float& value : HipOffset)
        {
            uint64_t bits = 0;
            aReader.ReadBits(bits, 32);
            const uint32_t raw = static_cast<uint32_t>(bits);
            std::memcpy(&value, &raw, sizeof(value));
        }
    }
    uint64_t hasWeapons = 0;
    aReader.ReadBits(hasWeapons, 1);
    HasWeapons = hasWeapons != 0;
    WeaponHeld = {};
    if (HasWeapons)
    {
        for (size_t side = 0; side < 2; ++side)
        {
            uint64_t held = 0;
            aReader.ReadBits(held, 1);
            WeaponHeld[side] = held != 0;
            if (!WeaponHeld[side])
                continue;
            WeaponRotation[side].Deserialize(aReader);
            for (float& value : WeaponOffset[side])
            {
                uint64_t bits = 0;
                aReader.ReadBits(bits, 32);
                const uint32_t raw = static_cast<uint32_t>(bits);
                std::memcpy(&value, &raw, sizeof(value));
            }
        }
    }
}
