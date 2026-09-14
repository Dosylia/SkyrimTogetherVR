#pragma once

#ifdef SKYRIMVR

#include <OverlayApp.hpp>

#include <atomic>
#include <mutex>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct OverlayService;

namespace vr
{
class IVRSystem;
class IVROverlay;
}

/**
 * @brief Shows the Skyrim Together UI as a SteamVR dashboard overlay.
 *
 * There is no game window to draw the overlay into on VR, so CEF renders the page offscreen at a fixed size and
 * the pixels go to a SteamVR dashboard tab ("Skyrim Together", opened with the headset's system button). The
 * texture lives on our own D3D11 device, which keeps this independent of the game's renderer and render thread.
 * SteamVR's laser pointer sends mouse events, and its virtual keyboard handles text fields.
 */
struct VRDashboard final : OverlayApp::RenderProvider
{
    static constexpr int kWidth = 1600;
    static constexpr int kHeight = 900;

    VRDashboard() noexcept;
    ~VRDashboard() override;

    TP_NOCOPYMOVE(VRDashboard);

    // OverlayApp::RenderProvider
    TiltedPhoques::OverlayRenderHandler* Create() override;
    HWND GetWindow() override { return nullptr; }

    /**
     * @brief Uploads new page frames, and forwards dashboard input to the page. Main thread, every frame.
     */
    void Update(OverlayService& aOverlay) noexcept;

    // Written by CEF's paint thread, read by Update.
    struct Frame
    {
        std::mutex Lock;
        std::vector<uint8_t> Pixels;
        bool Dirty = false;
    };

    Frame& GetFrame() noexcept { return m_frame; }
    void RequestKeyboard(bool aShow) noexcept { m_keyboardRequest = aShow ? 1 : 2; }

private:
    bool InitializeOverlay() noexcept;
    void UploadFrame() noexcept;
    void ProcessEvents(OverlayService& aOverlay) noexcept;
    void InjectText(OverlayApp& aApp, const char* acpUtf8) noexcept;

    Frame m_frame;
    std::atomic<int> m_keyboardRequest{0}; // 0 nothing, 1 show, 2 hide

    vr::IVRSystem* m_pSystem = nullptr;
    vr::IVROverlay* m_pOverlay = nullptr;
    uint64_t m_handle = 0;
    uint64_t m_thumbnailHandle = 0;

    ID3D11Device* m_pDevice = nullptr;
    ID3D11DeviceContext* m_pContext = nullptr;
    ID3D11Texture2D* m_pTexture = nullptr;

    bool m_initialized = false;
    bool m_failed = false;
    bool m_visible = false;
    bool m_browserHidden = false;
    uint16_t m_mouseX = 0;
    uint16_t m_mouseY = 0;
};

#endif
