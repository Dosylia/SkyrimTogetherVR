#include <TiltedOnlinePCH.h>

#include <Games/Skyrim/VRHaptics.h>

#ifdef SKYRIMVR

#include <openvr/openvr.h>

#include <chrono>

namespace
{
using namespace std::chrono_literals;

// The game has already brought OpenVR up through its own openvr_api.dll, exactly as VRDashboard does. Asking that
// DLL shares the game's session; loading a second copy would not be connected to SteamVR at all.
vr::IVRSystem* System() noexcept
{
    static vr::IVRSystem* s_pSystem = nullptr;
    static bool s_tried = false;

    if (s_pSystem)
        return s_pSystem;

    // OpenVR can come up after the first frames, so a failure is retried for a while rather than latched. Once a
    // minute of trying has passed with nothing, stop asking: the answer is not going to change.
    static std::chrono::steady_clock::time_point s_giveUpAt{};
    const auto now = std::chrono::steady_clock::now();
    if (!s_tried)
    {
        s_tried = true;
        s_giveUpAt = now + 60s;
    }
    if (now >= s_giveUpAt)
        return nullptr;

    HMODULE hOpenVR = GetModuleHandleW(L"openvr_api.dll");
    if (!hOpenVR)
        return nullptr;

    using TGetGenericInterface = void*(const char*, vr::EVRInitError*);
    auto* pGetInterface = reinterpret_cast<TGetGenericInterface*>(GetProcAddress(hOpenVR, "VR_GetGenericInterface"));
    if (!pGetInterface)
        return nullptr;

    vr::EVRInitError error = vr::VRInitError_None;
    s_pSystem = static_cast<vr::IVRSystem*>(pGetInterface(vr::IVRSystem_Version, &error));
    return s_pSystem;
}
} // namespace

namespace VRHaptics
{
bool Pulse(const bool aRightHand, uint16_t aDurationMicroSec) noexcept
{
    vr::IVRSystem* pSystem = System();
    if (!pSystem)
        return false;

    const vr::TrackedDeviceIndex_t device =
        pSystem->GetTrackedDeviceIndexForControllerRole(aRightHand ? vr::TrackedControllerRole_RightHand : vr::TrackedControllerRole_LeftHand);
    if (device == vr::k_unTrackedDeviceIndexInvalid)
        return false;

    // A single call is capped at just under 4 ms of vibration by OpenVR; anything longer is silently truncated,
    // so it is clamped here instead of pretending it was asked for.
    if (aDurationMicroSec > 3999)
        aDurationMicroSec = 3999;

    pSystem->TriggerHapticPulse(device, 0, aDurationMicroSec);
    return true;
}

} // namespace VRHaptics

#else

namespace VRHaptics
{
bool Pulse(bool, uint16_t) noexcept
{
    return false;
}
} // namespace VRHaptics

#endif
