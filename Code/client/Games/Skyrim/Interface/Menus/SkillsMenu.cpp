
// The game calls it statsmenu.
#include <Interface/Menus/SkillsMenu.h>

static TiltedPhoques::Initializer s_skillsMenuInit(
    []()
    {
        // https://github.com/Vermunds/SkyrimSoulsRE/blob/master/src/Menus/StatsMenuEx.cpp
        // Hoooks from souls RE
        // Fix for menu not appearing
        #ifndef SKYRIMVR
        VersionDbPtr<uint8_t> ProcessMessage(52510);
        #else
        VersionDbPtr<uint8_t> ProcessMessage(0); // TODOVR : find the correct id for VR
        #endif
        TiltedPhoques::Nop(ProcessMessage.Get() + 0x84E, 6);
        // Prevent setting kFreezeFrameBackground flag
        TiltedPhoques::Nop(ProcessMessage.Get() + 0xA10, 4);
        // Keep the menu updated
        TiltedPhoques::Nop(ProcessMessage.Get() + 0x1040, 2);

        // Fix for controls not working
        #ifndef SKYRIMVR
        VersionDbPtr<uint8_t> controlPatch(52518);
        #else
        VersionDbPtr<uint8_t> controlPatch(0); // TODOVR : find the correct id for VR
        #endif
        TiltedPhoques::Nop(controlPatch.Get() + 0x46, 4);
        TiltedPhoques::Nop(controlPatch.Get() + 0x4A, 2);
    });
