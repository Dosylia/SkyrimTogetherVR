#pragma once

// TODOVR: matrix row/column convention (and therefore rotation handedness) is
// assumed here to match the standard NetImmerse/Gamebryo layout used by every
// public SE/VR reverse-engineering reference, but hasn't been verified against
// a live game process. Sanity-check remote hand/head orientation in-game once
// testable, and fix the conversion in NiMatrix3ToQuat below if it looks wrong.
struct NiMatrix3
{
    float data[3][3];
};
static_assert(sizeof(NiMatrix3) == 0x24);

struct NiTransform
{
    NiMatrix3 rotate;
    NiPoint3 translate;
    float scale;
};

inline glm::quat NiMatrix3ToQuat(const NiMatrix3& aMatrix) noexcept
{
    const float(&m)[3][3] = aMatrix.data;
    const float trace = m[0][0] + m[1][1] + m[2][2];

    glm::quat q{};
    if (trace > 0.f)
    {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m[2][1] - m[1][2]) / s;
        q.y = (m[0][2] - m[2][0]) / s;
        q.z = (m[1][0] - m[0][1]) / s;
    }
    else if (m[0][0] > m[1][1] && m[0][0] > m[2][2])
    {
        const float s = std::sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
        q.w = (m[2][1] - m[1][2]) / s;
        q.x = 0.25f * s;
        q.y = (m[0][1] + m[1][0]) / s;
        q.z = (m[0][2] + m[2][0]) / s;
    }
    else if (m[1][1] > m[2][2])
    {
        const float s = std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
        q.w = (m[0][2] - m[2][0]) / s;
        q.x = (m[0][1] + m[1][0]) / s;
        q.y = 0.25f * s;
        q.z = (m[1][2] + m[2][1]) / s;
    }
    else
    {
        const float s = std::sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
        q.w = (m[1][0] - m[0][1]) / s;
        q.x = (m[0][2] + m[2][0]) / s;
        q.y = (m[1][2] + m[2][1]) / s;
        q.z = 0.25f * s;
    }

    return glm::normalize(q);
}
