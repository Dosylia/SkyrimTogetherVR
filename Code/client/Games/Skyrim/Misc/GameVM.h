#pragma once

#include <Misc/BSScript.h>

// The merge of 2026-09-22 took upstream's Anniversary Edition layout for this structure, which moved
// virtualMachine from 0x200 to 0x210 and inactive from 0x680 (a byte) to 0x690 (a dword). VR uses the
// Special Edition layout, so both had to stay where they were. Taking the AE offsets read a garbage
// pointer for virtualMachine and killed the game while Papyrus scripts were being bound at save load
// (BSScript::IsRemotePlayerFunc's constructor, 2026-09-22 21:44); inactive is worse than it looks,
// because it is the flag HookVMUpdate tests before running the whole mod's update.
struct SkyrimVM
{
    virtual ~SkyrimVM();

    static SkyrimVM* Get();

#ifdef SKYRIMVR
    uint8_t pad8[0x200 - 0x8];
    BSScript::IVirtualMachine* virtualMachine;
    uint8_t pad208[0x680 - 0x208];
    uint8_t inactive;
#else
    uint8_t pad8[0x210 - 0x8];
    BSScript::IVirtualMachine* virtualMachine;
    uint8_t pad218[0x690 - 0x218];
    int32_t inactive;
#endif
};

#ifdef SKYRIMVR
static_assert(offsetof(SkyrimVM, virtualMachine) == 0x200);
static_assert(offsetof(SkyrimVM, inactive) == 0x680);
#else
static_assert(offsetof(SkyrimVM, virtualMachine) == 0x210);
static_assert(offsetof(SkyrimVM, inactive) == 0x690);
#endif

using GameVM = SkyrimVM;
