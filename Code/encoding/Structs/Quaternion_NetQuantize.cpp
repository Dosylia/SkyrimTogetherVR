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

uint32_t Quaternion_NetQuantize::Pack() const noexcept
{
    glm::quat q = glm::normalize(static_cast<glm::quat>(*this));

    uint32_t largestIndex = 0;
    float largestValue = std::abs(q.x);
    float components[4] = {q.x, q.y, q.z, q.w};
    for (uint32_t i = 1; i < 4; ++i)
    {
        if (std::abs(components[i]) > largestValue)
        {
            largestValue = std::abs(components[i]);
            largestIndex = i;
        }
    }

    // Negate so the dropped (largest) component is always positive; q and -q
    // represent the same rotation so this loses no information.
    if (components[largestIndex] < 0.f)
    {
        for (auto& c : components)
            c = -c;
    }

    uint32_t data = largestIndex & 0x3;
    uint32_t shift = 2;
    for (uint32_t i = 0; i < 4; ++i)
    {
        if (i == largestIndex)
            continue;

        const float clamped = TiltedPhoques::Clamp(components[i], -cMaxComponent, cMaxComponent);
        const auto quantized = static_cast<uint32_t>((clamped + cMaxComponent) * cScalingFactor) & cComponentMax;
        data |= quantized << shift;
        shift += cComponentBits;
    }

    return data;
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
