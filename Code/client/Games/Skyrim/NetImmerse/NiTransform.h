#pragma once

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
