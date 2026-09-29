#include <Structs/Quaternion_NetQuantize.h>
#include <TiltedCore/Math.hpp>
#include <TiltedCore/Serialization.hpp>

using TiltedPhoques::Serialization;

// 10 bits per component, range [-1/sqrt(2), 1/sqrt(2)] since the dropped
// (largest) component is always >= 1/sqrt(2) for a normalized quaternion.
constexpr float cMaxComponent = 0.70710678f; // 1/sqrt(2)
constexpr uint32_t cComponentBits = 10;
constexpr uint32_t cComponentMax = (1u << cComponentBits) - 1;
constexpr float cScalingFactor = float(cComponentMax) / (2.0f * cMaxComponent);

bool Quaternion_NetQuantize::operator==(const Quaternion_NetQuantize& acRhs) const noexcept
{
    return Pack() == acRhs.Pack();
}

bool Quaternion_NetQuantize::operator!=(const Quaternion_NetQuantize& acRhs) const noexcept
{
    return !this->operator==(acRhs);
}

Quaternion_NetQuantize& Quaternion_NetQuantize::operator=(const glm::quat& acRhs) noexcept
{
    glm::quat::operator=(acRhs);
    return *this;
}

void Quaternion_NetQuantize::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Pack(), 32);
}

void Quaternion_NetQuantize::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    uint64_t data = 0;
    aReader.ReadBits(data, 32);

    Unpack(static_cast<uint32_t>(data & 0xFFFFFFFF));
}

namespace
{
//! Quantise a quaternion, having already been told which component to drop.
uint32_t PackWithLargest(const glm::quat& acQuat, const uint32_t aLargestIndex) noexcept
{
    float components[4] = {acQuat.x, acQuat.y, acQuat.z, acQuat.w};

    // Negate so the dropped (largest) component is always positive; q and -q represent the same rotation so
    // this loses no information.
    if (components[aLargestIndex] < 0.f)
    {
        for (auto& c : components)
            c = -c;
    }

    uint32_t data = aLargestIndex & 0x3;
    uint32_t shift = 2;
    for (uint32_t i = 0; i < 4; ++i)
    {
        if (i == aLargestIndex)
            continue;

        const float clamped = TiltedPhoques::Clamp(components[i], -cMaxComponent, cMaxComponent);

        // Round to the nearest bucket rather than truncating towards zero. Truncating cost half a bucket of
        // accuracy on every component, and worse, it was not stable: a component decoded out of bucket 75 comes
        // back as 74.9999847 in float and truncates into bucket 74, so a value lost a bucket every time it went
        // through the codec. Relayed poses drifted, and the shifting components changed which one was largest,
        // which is what made packing oscillate.
        const float scaled = (clamped + cMaxComponent) * cScalingFactor + 0.5f;
        const auto quantized = static_cast<uint32_t>(TiltedPhoques::Clamp(scaled, 0.f, float(cComponentMax))) & cComponentMax;
        data |= quantized << shift;
        shift += cComponentBits;
    }

    return data;
}

uint32_t LargestIndexOf(const glm::quat& acQuat) noexcept
{
    const float components[4] = {acQuat.x, acQuat.y, acQuat.z, acQuat.w};
    uint32_t largest = 0;
    float largestValue = std::abs(components[0]);
    for (uint32_t i = 1; i < 4; ++i)
    {
        if (std::abs(components[i]) > largestValue)
        {
            largestValue = std::abs(components[i]);
            largest = i;
        }
    }
    return largest;
}
} // namespace

uint32_t Quaternion_NetQuantize::Pack() const noexcept
{
    const glm::quat q = glm::normalize(static_cast<glm::quat>(*this));

    uint32_t data = PackWithLargest(q, LargestIndexOf(q));

    // Settle on an encoding that survives its own round trip.
    //
    // "Smallest three" drops whichever component is largest. When two components are nearly equal in magnitude
    // -- (-0.6034, 0.6034, 0.4022, 0.3318) is a real example -- quantising the other three moves them just
    // enough that the *decoded* quaternion has a different largest component. Packing that decoded value drops
    // a different component, and since the new one is negative the whole quaternion is negated: a completely
    // different set of bits for the same rotation.
    //
    // That matters because Pack() is what equality is built on. VRBodySync only sends a pose when the quantised
    // bones differ from the ones it last sent, and VRPose::operator== compares packed forms. A bone sitting near
    // such a tie would be reported as changed on every frame, for ever, while never actually moving.
    //
    // So walk the encodings until one agrees with itself -- decoding it picks the component it already dropped.
    // Near a tie there may be no such encoding, only a short cycle of them, all describing the same rotation to
    // within the quantisation step. In that case take the lowest, which every member of the cycle can work out
    // for itself, so they all settle on the same answer.
    constexpr size_t cMaxCycle = 8;
    uint32_t seen[cMaxCycle] = {};
    size_t count = 0;

    while (count < cMaxCycle)
    {
        Quaternion_NetQuantize decoded;
        decoded.Unpack(data);

        const uint32_t settledIndex = LargestIndexOf(decoded);
        if (settledIndex == (data & 0x3))
            return data;

        bool alreadySeen = false;
        for (size_t i = 0; i < count; ++i)
            alreadySeen = alreadySeen || seen[i] == data;

        if (alreadySeen)
            break;

        seen[count++] = data;
        data = PackWithLargest(glm::normalize(static_cast<glm::quat>(decoded)), settledIndex);
    }

    uint32_t lowest = data;
    for (size_t i = 0; i < count; ++i)
    {
        if (seen[i] < lowest)
            lowest = seen[i];
    }

    return lowest;
}

void Quaternion_NetQuantize::Unpack(uint32_t aValue) noexcept
{
    const uint32_t largestIndex = aValue & 0x3;

    float components[4] = {};
    uint32_t shift = 2;
    float sumOfSquares = 0.f;
    for (uint32_t i = 0; i < 4; ++i)
    {
        if (i == largestIndex)
            continue;

        const uint32_t quantized = (aValue >> shift) & cComponentMax;
        shift += cComponentBits;

        const float value = (static_cast<float>(quantized) / cScalingFactor) - cMaxComponent;
        components[i] = value;
        sumOfSquares += value * value;
    }

    components[largestIndex] = std::sqrt(TiltedPhoques::Max(0.f, 1.0f - sumOfSquares));

    x = components[0];
    y = components[1];
    z = components[2];
    w = components[3];
}
