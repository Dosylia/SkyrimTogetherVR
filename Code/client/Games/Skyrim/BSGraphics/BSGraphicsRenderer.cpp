
#include "Services/InputService.h"
#include "Systems/RenderSystemD3D11.h"

#include "World.h"

#include "BSGraphics/BSGraphicsRenderer.h"
#include "BSRandom/BSRandom.h"

// shared resource by launcher
extern HICON g_SharedWindowIcon;

namespace BSGraphics
{
namespace
{

static RenderSystemD3D11* g_sRs = nullptr;
static WNDPROC RealWndProc = nullptr;
static RendererWindow* g_RenderWindow = nullptr;

static constexpr char kTogetherWindowName[]{"Skyrim Together"};

} // namespace
RendererWindow* GetMainWindow()
{
    return g_RenderWindow;
}

bool RendererWindow::IsForeground()
{
    return GetForegroundWindow() == hWnd;
}

void (*Renderer_Init)(Renderer*, BSGraphics::RendererInitOSData*, const BSGraphics::ApplicationWindowProperties*, BSGraphics::RendererInitReturn*) = nullptr;

// WNDPROC seems to be part of the renderer
LRESULT CALLBACK Hook_WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (InputService::WndProc(hwnd, uMsg, wParam, lParam) != 0)
        return 0;

    return RealWndProc(hwnd, uMsg, wParam, lParam);
}

void Hook_Renderer_Init(Renderer* self, BSGraphics::RendererInitOSData* aOSData, const BSGraphics::ApplicationWindowProperties* aFBData, BSGraphics::RendererInitReturn* aOut)
{
    // we feed this a shared icon as the resource directory of our former launcher data is already overwritten with the
    // game.
    aOSData->hIcon = g_SharedWindowIcon;
    // Append our window name.
    aOSData->pClassName = kTogetherWindowName;

    RealWndProc = aOSData->pWndProc;
    aOSData->pWndProc = Hook_WndProc;

    Renderer_Init(self, aOSData, aFBData, aOut);

    g_sRs = &World::Get().ctx().at<RenderSystemD3D11>();
    // This how the game does it too
    g_RenderWindow = &self->Data.RenderWindowA[0];

    const BSGraphics::RendererData& renderer = self->Data;

    g_sRs->OnDeviceCreation(renderer.RenderWindowA[0].pSwapChain, renderer.pForwarder, renderer.pContext);
}

void (*StopTimer)(int) = nullptr;

// Insert us at the End
void Hook_StopTimer(int type)
{
    if (g_sRs)
        g_sRs->OnRender();

    StopTimer(type);
}

static TiltedPhoques::Initializer s_viewportHooks(
    []()
    {
        // ids 77226/68781/77246 don't exist in the official VR Address
        // Library at all - only in the unverified crosswalk table. 77226 in
        // particular is used both for a raw byte patch AND to hook the
        // *entire* Renderer_Init function via TP_HOOK_IMMEDIATE - if it
        // resolves to an unrelated function on VR (likely, since it's
        // unverified), our hook still installs (the id is non-null, just
        // wrong), and Hook_Renderer_Init then misinterprets whatever real
        // parameters that unrelated function receives as
        // RendererInitOSData*/ApplicationWindowProperties*, writing through
        // those pointers as if they were the real struct. That's silent
        // corruption with no guaranteed crash - a strong suspect for VR menus
        // getting stuck (logo renders, UI text never finishes initializing).
        // Disabled entirely on VR until these ids are verified: this drops
        // the window-style patch, the DirectInput exclusivity patch, and the
        // mod's own overlay/imgui device-creation hookup (OnDeviceCreation
        // never fires), but leaves the game's own renderer untouched.
#ifndef SKYRIMVR
        const VersionDbPtr<void> initWindowLoc(77226);
        TiltedPhoques::Put(mem::pointer(initWindowLoc.GetPtr()) + 0x174 + 1, WS_OVERLAPPEDWINDOW);
        const VersionDbPtr<void> windowLoc(68781);
        TiltedPhoques::Put(
            mem::pointer(windowLoc.GetPtr()) + 0x55 + 2,
            /*strip DISCL_EXCLUSIVE bits and append DISCL_NONEXCLUSIVE*/ 3);
        const VersionDbPtr<void> timerLoc(77246);
        const VersionDbPtr<void> renderInit(77226);
        TiltedPhoques::SwapCall(mem::pointer(timerLoc.GetPtr()) + 9, StopTimer, &Hook_StopTimer);
        Renderer_Init = static_cast<decltype(Renderer_Init)>(renderInit.GetPtr());
        TP_HOOK_IMMEDIATE(&Renderer_Init, &Hook_Renderer_Init);
#endif
    });
} // namespace BSGraphics
