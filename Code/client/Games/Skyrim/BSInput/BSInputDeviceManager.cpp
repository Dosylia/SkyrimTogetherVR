
#include <BSGraphics/BSGraphicsRenderer.h>

struct BSInputDeviceManager;

void (*BSInputDeviceManager_PollInputDevices)(BSInputDeviceManager*, float) = nullptr;

void Hook_BSInputDeviceManager_PollInputDevices(BSInputDeviceManager* inputDeviceMgr, float afDelta)
{
    // Null on VR, where Hook_Renderer_Init is off.
    auto* pWindow = BSGraphics::GetMainWindow();
    if (pWindow && !pWindow->IsForeground())
        return;

    BSInputDeviceManager_PollInputDevices(inputDeviceMgr, afDelta);
}

static TiltedPhoques::Initializer s_initInputDeviceManager(
    []()
    {
        #ifndef SKYRIMVR
        const VersionDbPtr<void> pollInputDevices(68617);

        BSInputDeviceManager_PollInputDevices = static_cast<decltype(BSInputDeviceManager_PollInputDevices)>(pollInputDevices.GetPtr());

        TP_HOOK_IMMEDIATE(&BSInputDeviceManager_PollInputDevices, &Hook_BSInputDeviceManager_PollInputDevices);
        #endif
        // Off on VR on purpose, although the address is confirmed (SE 67315, VR 0xc519e0). The desktop mirror window is
        // rarely focused while somebody is in a headset, so skipping input polling when it isn't would stop the
        // controllers. It would also do nothing today: the main window is null on VR, where Hook_Renderer_Init is off.
    });
