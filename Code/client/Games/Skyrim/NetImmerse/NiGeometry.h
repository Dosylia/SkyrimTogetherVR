#pragma once

#include <NetImmerse/NiPointer.h>
#include <NetImmerse/NiAVObject.h>
#include <NetImmerse/NiProperty.h>

struct NiGeometry : NiAVObject
{
    virtual ~NiGeometry();

#ifdef SKYRIMVR
    // VR NiAVObject is bigger (0x138), so the geometry properties start at 0x160.
    uint8_t vrPad110[0x160 - 0x110];
    NiPointer<NiProperty> unkProperty1; // 160 alpha property
    NiPointer<NiProperty> effect;       // 168 shader property
#else
    NiPointer<NiProperty> unkProperty1;
    NiPointer<NiProperty> unkProperty2;
    uintptr_t unkB0;
    NiPointer<NiProperty> effect;
    uintptr_t unkB8;
#endif
};

#ifdef SKYRIMVR
static_assert(offsetof(NiGeometry, effect) == 0x168);
#endif
