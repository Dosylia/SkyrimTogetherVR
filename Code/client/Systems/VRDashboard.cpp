#include <TiltedOnlinePCH.h>

#ifdef SKYRIMVR

#include <Systems/VRDashboard.h>

#include <Services/OverlayService.h>

#include <OverlayClient.hpp>
#include <OverlayRenderHandler.hpp>

#include <openvr/openvr.h>

#include <d3d11.h>
#include <dxgi.h>

#include <fstream>

namespace
{
// CEF paints into the dashboard's shared frame buffer. Everything touching D3D11 or OpenVR happens in
// VRDashboard::Update on the main thread.
struct DashboardRenderHandler final : TiltedPhoques::OverlayRenderHandler
{
    explicit DashboardRenderHandler(VRDashboard& aDashboard)
        : m_dashboard(aDashboard)
    {
        SetVisible(true);
    }

    void Create() override {}
    void Render() override {}
    void Reset() override {}

    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& aRect) override { aRect = CefRect(0, 0, VRDashboard::kWidth, VRDashboard::kHeight); }

    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType aType, const RectList&, const void* apBuffer, int aWidth, int aHeight) override
    {
        static std::atomic<bool> s_loggedFirstPaint = false;
        if (!s_loggedFirstPaint.exchange(true))
            spdlog::info("VRDashboard: first page paint, type {}, {}x{}", static_cast<int>(aType), aWidth, aHeight);

        if (aType != PET_VIEW || aWidth != VRDashboard::kWidth || aHeight != VRDashboard::kHeight)
            return;

        auto& frame = m_dashboard.GetFrame();
        std::scoped_lock lock(frame.Lock);
        std::memcpy(frame.Pixels.data(), apBuffer, frame.Pixels.size());
        frame.Dirty = true;
    }

    void OnVirtualKeyboardRequested(CefRefPtr<CefBrowser>, TextInputMode aInputMode) override { m_dashboard.RequestKeyboard(aInputMode != CEF_TEXT_INPUT_MODE_NONE); }

    IMPLEMENT_REFCOUNTING(DashboardRenderHandler);

private:
    VRDashboard& m_dashboard;
};

void SafeRelease(IUnknown*& apObject) noexcept
{
    if (apObject)
    {
        apObject->Release();
        apObject = nullptr;
    }
}
} // namespace

VRDashboard::VRDashboard() noexcept
{
    m_frame.Pixels.resize(static_cast<size_t>(kWidth) * kHeight * 4);
}

VRDashboard::~VRDashboard()
{
    if (m_pOverlay && m_handle)
        m_pOverlay->DestroyOverlay(m_handle);

    SafeRelease(reinterpret_cast<IUnknown*&>(m_pTexture));
    SafeRelease(reinterpret_cast<IUnknown*&>(m_pContext));
    SafeRelease(reinterpret_cast<IUnknown*&>(m_pDevice));
}

TiltedPhoques::OverlayRenderHandler* VRDashboard::Create()
{
    return new DashboardRenderHandler(*this);
}

bool VRDashboard::InitializeOverlay() noexcept
{
    // The game has already initialized OpenVR through its own openvr_api.dll. Asking that DLL for the interfaces
    // shares the game's session; a second copy of the library would not be connected to SteamVR.
    HMODULE hOpenVR = GetModuleHandleW(L"openvr_api.dll");
    if (!hOpenVR)
        return false;

    using TGetGenericInterface = void*(const char*, vr::EVRInitError*);
    auto* pGetInterface = reinterpret_cast<TGetGenericInterface*>(GetProcAddress(hOpenVR, "VR_GetGenericInterface"));
    if (!pGetInterface)
        return false;

    vr::EVRInitError error = vr::VRInitError_None;
    m_pSystem = static_cast<vr::IVRSystem*>(pGetInterface(vr::IVRSystem_Version, &error));
    m_pOverlay = static_cast<vr::IVROverlay*>(pGetInterface(vr::IVROverlay_Version, &error));
    if (!m_pSystem || !m_pOverlay)
    {
        spdlog::warn("VRDashboard: OpenVR interfaces unavailable (error {})", static_cast<int>(error));
        return false;
    }

    // The texture must be created on the adapter the headset is connected to.
    int32_t adapterIndex = -1;
    m_pSystem->GetDXGIOutputInfo(&adapterIndex);

    IDXGIFactory1* pFactory = nullptr;
    IDXGIAdapter1* pAdapter = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&pFactory))) && adapterIndex >= 0)
        pFactory->EnumAdapters1(adapterIndex, &pAdapter);

    const HRESULT deviceResult = D3D11CreateDevice(pAdapter, pAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &m_pDevice, nullptr, &m_pContext);

    if (pAdapter)
        pAdapter->Release();
    if (pFactory)
        pFactory->Release();

    if (FAILED(deviceResult))
    {
        spdlog::error("VRDashboard: D3D11CreateDevice failed ({:#x})", static_cast<uint32_t>(deviceResult));
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // CEF paints BGRA
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED; // the SteamVR compositor opens it from its own process

    if (FAILED(m_pDevice->CreateTexture2D(&desc, nullptr, &m_pTexture)))
    {
        spdlog::error("VRDashboard: could not create the overlay texture");
        return false;
    }

    const auto overlayError = m_pOverlay->CreateDashboardOverlay("skyrimtogether.menu", "Skyrim Together", &m_handle, &m_thumbnailHandle);
    if (overlayError != vr::VROverlayError_None)
    {
        spdlog::error("VRDashboard: CreateDashboardOverlay failed ({})", static_cast<int>(overlayError));
        return false;
    }

    m_pOverlay->SetOverlayWidthInMeters(m_handle, 2.5f);
    m_pOverlay->SetOverlayInputMethod(m_handle, vr::VROverlayInputMethod_Mouse);

    const vr::HmdVector2_t mouseScale{{static_cast<float>(kWidth), static_cast<float>(kHeight)}};
    m_pOverlay->SetOverlayMouseScale(m_handle, &mouseScale);
    m_pOverlay->SetOverlayFlag(m_handle, vr::VROverlayFlags_SendVRSmoothScrollEvents, true);

    // Tab icon, drawn here so it needs no image file: a gold Skyrim-style diamond with a dark centre on a dark
    // rounded square, readable at the dashboard's thumbnail size. The plain gold square it replaces was asked
    // to become a proper icon on 2026-09-19.
    constexpr int32_t kIconSize = 64;
    std::vector<uint8_t> icon(static_cast<size_t>(kIconSize) * kIconSize * 4);
    for (int32_t y = 0; y < kIconSize; ++y)
    {
        for (int32_t x = 0; x < kIconSize; ++x)
        {
            const float cx = static_cast<float>(x) - 31.5f;
            const float cy = static_cast<float>(y) - 31.5f;
            const float diamond = std::abs(cx) + std::abs(cy); // L1 distance from the centre
            const float corner = std::max(std::abs(cx), std::abs(cy));

            uint8_t r = 24, g = 21, b = 18, a = 255; // dark panel
            if (corner > 30.f)
                a = 0; // rounded off corners
            if (diamond <= 26.f && diamond > 21.f)
            {
                r = 200; // gold ring
                g = 169;
                b = 110;
            }
            else if (diamond <= 13.f && diamond > 9.f)
            {
                r = 200; // inner ring
                g = 169;
                b = 110;
            }

            uint8_t* pPixel = icon.data() + (static_cast<size_t>(y) * kIconSize + x) * 4;
            pPixel[0] = r;
            pPixel[1] = g;
            pPixel[2] = b;
            pPixel[3] = a;
        }
    }
    m_pOverlay->SetOverlayRaw(m_thumbnailHandle, icon.data(), kIconSize, kIconSize, 4);

    spdlog::info("VRDashboard: Skyrim Together dashboard overlay created");
    return true;
}

void VRDashboard::Update(OverlayService& aOverlay) noexcept
{
    if (m_failed)
        return;

    if (!m_initialized)
    {
        // OpenVR can come up after the first frames; try a few times before giving up.
        static int s_attempts = 0;
        static std::chrono::steady_clock::time_point s_nextAttempt;
        const auto now = std::chrono::steady_clock::now();
        if (now < s_nextAttempt)
            return;

        s_nextAttempt = now + std::chrono::seconds(2);
        m_initialized = InitializeOverlay();
        if (!m_initialized && ++s_attempts >= 10)
        {
            spdlog::error("VRDashboard: giving up, the Skyrim Together menu is not available in the headset");
            m_failed = true;
        }
        if (!m_initialized)
            return;
    }

    ProcessEvents(aOverlay);

    // The page only renders while the dashboard tab is open. Rendering it all the time cost frame time for a
    // menu nobody was looking at.
    if (OverlayApp* pApp = aOverlay.GetOverlayApp())
    {
        const auto pBrowser = pApp->GetClient() ? pApp->GetClient()->GetBrowser() : nullptr;
        if (pBrowser && pBrowser->GetHost() && m_browserHidden == m_visible)
        {
            m_browserHidden = !m_visible;
            pBrowser->GetHost()->WasHidden(m_browserHidden);

            // The page is the flat-screen UI at 1600x900: on a 2.5 m panel its menu was a small box in one corner
            // (2026-09-19). Chrome zoom scales everything on it; each level is x1.2, so 3 is about 173%.
            static bool s_zoomed = false;
            if (m_visible && !s_zoomed)
            {
                s_zoomed = true;
                constexpr double kZoomLevel = 3.0;
                pBrowser->GetHost()->SetZoomLevel(kZoomLevel);
                spdlog::info("VRDashboard: page zoom level set to {}", kZoomLevel);
            }
        }
    }

    if (m_visible)
    {
        UploadFrame();
        SnapshotPage();
    }

    const int keyboardRequest = m_keyboardRequest.exchange(0);
    if (keyboardRequest == 1)
        m_pOverlay->ShowKeyboardForOverlay(m_handle, vr::k_EGamepadTextInputModeNormal, vr::k_EGamepadTextInputLineModeSingleLine, vr::KeyboardFlag_Minimal, "Skyrim Together", 256, "", 0);
    else if (keyboardRequest == 2)
        m_pOverlay->HideKeyboard();
}

void VRDashboard::UploadFrame() noexcept
{
    constexpr uint8_t kBackground[3]{24, 21, 18}; // BGR, dark panel behind the page

    size_t visiblePixels = 0;
    {
        std::scoped_lock lock(m_frame.Lock);
        if (!m_frame.Dirty)
            return;

        // CEF paints premultiplied BGRA, and the UI is made to sit over the game, so most of the page is transparent.
        // The dashboard would show that as an empty tab: blend it over an opaque panel instead.
        m_opaquePixels.resize(m_frame.Pixels.size());
        const uint8_t* pSource = m_frame.Pixels.data();
        uint8_t* pTarget = m_opaquePixels.data();
        for (size_t i = 0; i < m_frame.Pixels.size(); i += 4)
        {
            const uint32_t alpha = pSource[i + 3];
            const uint32_t inverse = 255 - alpha;
            for (int c = 0; c < 3; ++c)
                pTarget[i + c] = static_cast<uint8_t>(std::min<uint32_t>(255, pSource[i + c] + (kBackground[c] * inverse + 127) / 255));
            pTarget[i + 3] = 255;
            visiblePixels += alpha != 0;
        }

        m_pContext->UpdateSubresource(m_pTexture, 0, nullptr, m_opaquePixels.data(), kWidth * 4, 0);
        m_frame.Dirty = false;
    }

    // Nothing ever presents on this device, so D3D11 would keep the upload queued indefinitely and SteamVR would
    // read a blank shared texture. Flush so the pixels are on the GPU before the compositor opens it.
    m_pContext->Flush();

    vr::Texture_t texture{m_pTexture, vr::TextureType_DirectX, vr::ColorSpace_Auto};
    const auto error = m_pOverlay->SetOverlayTexture(m_handle, &texture);

    if (m_logNextUpload)
    {
        m_logNextUpload = false;
        spdlog::info("VRDashboard: page frame sent to SteamVR (result {}), {}% of the page has content", static_cast<int>(error),
                     visiblePixels * 100 / (static_cast<size_t>(kWidth) * kHeight));
    }
}

// TEMPORARY menu diagnostic: the tab showed an empty panel. Two seconds after it opens, save what the page drew to
// logs\dashboard_frame.bmp (once per session) and log how much of it has content. Remove once the menu works.
void VRDashboard::SnapshotPage() noexcept
{
    if (m_snapshotTaken || std::chrono::steady_clock::now() - m_shownAt < std::chrono::seconds(2))
        return;
    m_snapshotTaken = true;

    std::vector<uint8_t> pixels;
    {
        std::scoped_lock lock(m_frame.Lock);
        pixels = m_frame.Pixels;
    }

    size_t visible = 0;
    uint32_t minX = kWidth, minY = kHeight, maxX = 0, maxY = 0;
    for (uint32_t y = 0; y < kHeight; ++y)
    {
        for (uint32_t x = 0; x < kWidth; ++x)
        {
            if (pixels[(static_cast<size_t>(y) * kWidth + x) * 4 + 3] == 0)
                continue;
            ++visible;
            minX = std::min(minX, x);
            minY = std::min(minY, y);
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, y);
        }
    }

    const auto path = TiltedPhoques::GetPath() / "logs" / "dashboard_frame.bmp";
    std::ofstream file(path, std::ios::binary);
    if (file)
    {
        BITMAPFILEHEADER fileHeader{};
        BITMAPINFOHEADER infoHeader{};
        infoHeader.biSize = sizeof(infoHeader);
        infoHeader.biWidth = kWidth;
        infoHeader.biHeight = -kHeight; // top-down
        infoHeader.biPlanes = 1;
        infoHeader.biBitCount = 32;
        infoHeader.biCompression = BI_RGB;
        fileHeader.bfType = 0x4D42;
        fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(infoHeader);
        fileHeader.bfSize = fileHeader.bfOffBits + static_cast<DWORD>(pixels.size());
        file.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
        file.write(reinterpret_cast<const char*>(&infoHeader), sizeof(infoHeader));
        file.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    }

    if (visible)
        spdlog::info("VRDashboard: 2 s after opening, {}% of the page has content, between ({}, {}) and ({}, {}); saved to {}", visible * 100 / (static_cast<size_t>(kWidth) * kHeight),
                     minX, minY, maxX, maxY, path.string());
    else
        spdlog::info("VRDashboard: 2 s after opening, the page is still completely transparent (nothing drawn); saved to {}", path.string());
}

void VRDashboard::ProcessEvents(OverlayService& aOverlay) noexcept
{
    OverlayApp* pApp = aOverlay.GetOverlayApp();

    vr::VREvent_t event{};
    while (m_pOverlay->PollNextOverlayEvent(m_handle, &event, sizeof(event)))
    {
        switch (event.eventType)
        {
        case vr::VREvent_MouseMove:
        case vr::VREvent_MouseButtonDown:
        case vr::VREvent_MouseButtonUp:
        {
            // OpenVR mouse coordinates start at the bottom left, the page's at the top left.
            m_mouseX = static_cast<uint16_t>(std::clamp(event.data.mouse.x, 0.f, static_cast<float>(kWidth - 1)));
            m_mouseY = static_cast<uint16_t>(std::clamp(kHeight - event.data.mouse.y, 0.f, static_cast<float>(kHeight - 1)));

            if (!pApp)
                break;

            if (event.eventType == vr::VREvent_MouseMove)
            {
                pApp->InjectMouseMove(m_mouseX, m_mouseY, 0);
            }
            else
            {
                const auto button = event.data.mouse.button == vr::VRMouseButton_Right ? MBT_RIGHT : MBT_LEFT;
                pApp->InjectMouseButton(m_mouseX, m_mouseY, button, event.eventType == vr::VREvent_MouseButtonUp, 0);
            }
            break;
        }
        case vr::VREvent_ScrollSmooth:
            if (pApp)
                pApp->InjectMouseWheel(m_mouseX, m_mouseY, static_cast<int16_t>(event.data.scroll.ydelta * 120.f), 0);
            break;
        case vr::VREvent_KeyboardCharInput:
            if (pApp)
                InjectText(*pApp, event.data.keyboard.cNewInput);
            break;
        case vr::VREvent_OverlayShown:
        {
            spdlog::info("VRDashboard: tab opened (in game: {})", aOverlay.GetInGame());
            m_visible = true;
            m_logNextUpload = true;
            m_shownAt = std::chrono::steady_clock::now();
            aOverlay.SetActive(true);
            // CEF may not repaint an unchanged page when it is shown again: resend the last frame.
            std::scoped_lock lock(m_frame.Lock);
            m_frame.Dirty = true;
            break;
        }
        case vr::VREvent_OverlayHidden:
            m_visible = false;
            aOverlay.SetActive(false);
            break;
        default: break;
        }
    }
}

void VRDashboard::InjectText(OverlayApp& aApp, const char* acpUtf8) noexcept
{
    char utf8[9]{};
    std::memcpy(utf8, acpUtf8, 8);

    wchar_t text[9]{};
    const int count = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, text, 9);

    for (int i = 0; i < count && text[i]; ++i)
    {
        const wchar_t character = text[i];

        // Editing keys arrive as control characters and need real key presses.
        if (character == L'\b' || character == L'\n' || character == L'\r')
        {
            const uint16_t key = character == L'\b' ? VK_BACK : VK_RETURN;
            aApp.InjectKey(KEYEVENT_RAWKEYDOWN, 0, key, 0);
            if (key == VK_RETURN)
                aApp.InjectKey(KEYEVENT_CHAR, 0, L'\r', 0);
            aApp.InjectKey(KEYEVENT_KEYUP, 0, key, 0);
            continue;
        }

        aApp.InjectKey(KEYEVENT_CHAR, 0, static_cast<uint16_t>(character), 0);
    }
}

#endif
