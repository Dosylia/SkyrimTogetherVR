
#include <BSGraphics/BSGraphicsRenderer.h>

struct BSInputDeviceManager;

void (*BSInputDeviceManager_PollInputDevices)(BSInputDeviceManager*, float) = nullptr;

void Hook_BSInputDeviceManager_PollInputDevices(BSInputDeviceManager* inputDeviceMgr, float afDelta)
{
    // GetMainWindow() is null on VR (Hook_Renderer_Init, which sets it, is
    // disabled there - see BSGraphicsRenderer.cpp). This runs every input
    // poll, so an unguarded dereference here would crash immediately and
    // constantly.
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
        #else
        // Not hooked on VR: 68617 only resolves through the crosswalk (0/69 accurate
        // for functions), and the hook forwards just (rcx, xmm1), so on a wrong
        // target the other argument registers would reach it clobbered. The hook
        // does nothing on VR anyway - GetMainWindow() is always null there.
        #endif
    });
