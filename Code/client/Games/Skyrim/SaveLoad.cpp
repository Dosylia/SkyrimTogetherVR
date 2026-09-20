#include <TiltedOnlinePCH.h>

#include <SaveLoad.h>

// TEMPORARY (2026-09-20): confirming a level-up put up "The save game is corrupt and cannot be loaded" and a loading
// screen (11:55, and the same at 07:58 before the drop confused things). Nothing in this client loads a save, so
// every load request is named here with the file and the caller, to see who asks and for which save.
TP_THIS_FUNCTION(TSaveLoadManagerLoad, bool, BGSSaveLoadManager, const char* apFileName, bool aCheckForMods, bool aUnk);
static TSaveLoadManagerLoad* RealSaveLoadManagerLoad = nullptr;

bool TP_MAKE_THISCALL(HookSaveLoadManagerLoad, BGSSaveLoadManager, const char* apFileName, bool aCheckForMods, bool aUnk)
{
    char caller[160] = "?";
    HMODULE hModule = nullptr;
    void* pReturn = _ReturnAddress();
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(pReturn), &hModule) && hModule)
    {
        char name[MAX_PATH] = {};
        GetModuleFileNameA(hModule, name, sizeof(name));
        const char* pBase = strrchr(name, 92);
        _snprintf_s(caller, sizeof(caller), _TRUNCATE, "%s+0x%llx", pBase ? pBase + 1 : name, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pReturn) - reinterpret_cast<uintptr_t>(hModule)));
    }
    else
        _snprintf_s(caller, sizeof(caller), _TRUNCATE, "SkyrimVR.exe+0x%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pReturn) - 0x140000000ull));

    spdlog::info("SaveLoad: load requested for '{}' (check mods {}, flag {}) from {}", apFileName ? apFileName : "(null)", aCheckForMods, aUnk, caller);
    const bool result = TiltedPhoques::ThisCall(RealSaveLoadManagerLoad, apThis, apFileName, aCheckForMods, aUnk);
    spdlog::info("SaveLoad: load of '{}' returned {}", apFileName ? apFileName : "(null)", result);
    return result;
}

// TEMPORARY (2026-09-20): Seenfront read the game (VR 1.4.15) around the hide that ends at the main menu. A queued
// request of type 0xD0000010, built by the constructor at SkyrimVR+0x594D80, can run the route
// 5910D0 -> 5924A0 -> 584910 -> 591E80 -> 168160 -> F1BF10 -> F1D0E0 -> F20750 -> AddMessage (the hide we log).
// Two producers build it: the save-warning dialog callback at +0x595B60 (response 1 queues it; it calls the
// constructor from +0x595BCF) and another result handler (calls it from +0x591964). The queue separates building
// the request from running it, so the producer never shows in the hide's stack; it is logged here at construction,
// with its caller, stack and raw register arguments, and the warning callback logs its response byte.
namespace
{
uintptr_t GameBase() noexcept
{
    return reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
}

void DescribeAddress(const void* apAddress, char* apOut, const size_t aOutSize) noexcept
{
    HMODULE hModule = nullptr;
    char name[MAX_PATH] = {};
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(apAddress), &hModule) && hModule &&
        GetModuleFileNameA(hModule, name, sizeof(name)))
    {
        const char* pBase = strrchr(name, 92);
        _snprintf_s(apOut, aOutSize, _TRUNCATE, "%s+0x%llx", pBase ? pBase + 1 : name, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(apAddress) - reinterpret_cast<uintptr_t>(hModule)));
        return;
    }
    _snprintf_s(apOut, aOutSize, _TRUNCATE, "<0x%llx>", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(apAddress)));
}

std::string CaptureStack() noexcept
{
    void* stack[16] = {};
    const USHORT count = RtlCaptureStackBackTrace(2, 16, stack, nullptr);
    std::string frames;
    for (USHORT i = 0; i < count; ++i)
    {
        char frame[160];
        DescribeAddress(stack[i], frame, sizeof(frame));
        frames += fmt::format("{}{}", i == 0 ? "" : " < ", frame);
    }
    return frames;
}

bool Readable(const void* apAddress, const size_t aBytes) noexcept
{
    MEMORY_BASIC_INFORMATION info{};
    if (!apAddress || !VirtualQuery(apAddress, &info, sizeof(info)))
        return false;
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    return reinterpret_cast<uintptr_t>(apAddress) + aBytes <= reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
}

std::string FirstWords(const void* apObject, const int aCount) noexcept
{
    if (!Readable(apObject, aCount * 8))
        return "(unreadable)";
    std::string words;
    const uint64_t* pWords = static_cast<const uint64_t*>(apObject);
    for (int i = 0; i < aCount; ++i)
        words += fmt::format("{}{:x}", i == 0 ? "" : " ", pWords[i]);
    return words;
}

constexpr uintptr_t kRequestCtorRva = 0x594D80;
constexpr uintptr_t kWarningCallbackRva = 0x595B60;

using TRequestCtor = void*(void* apThis, uint64_t a2, uint64_t a3, uint64_t a4);
using TWarningCallback = uint64_t(void* apThis, uint64_t aResponse, uint64_t a3, uint64_t a4);
TRequestCtor* RealRequestCtor = nullptr;
TWarningCallback* RealWarningCallback = nullptr;

void* HookRequestCtor(void* apThis, uint64_t a2, uint64_t a3, uint64_t a4)
{
    char caller[160];
    DescribeAddress(_ReturnAddress(), caller, sizeof(caller));
    void* pResult = RealRequestCtor(apThis, a2, a3, a4);
    spdlog::info("SaveLoad: request built (ctor +0x594D80) this {:x}, args {:x} {:x} {:x}, words after [{}], from {}; stack [{}]", reinterpret_cast<uintptr_t>(apThis), a2, a3, a4,
                 FirstWords(apThis, 6), caller, CaptureStack());
    return pResult;
}

uint64_t HookWarningCallback(void* apThis, uint64_t aResponse, uint64_t a3, uint64_t a4)
{
    char caller[160];
    DescribeAddress(_ReturnAddress(), caller, sizeof(caller));
    spdlog::info("SaveLoad: warning callback (+0x595B60) this {:x}, response {} (rdx {:x}), words [{}], from {}; stack [{}]", reinterpret_cast<uintptr_t>(apThis), aResponse & 0xFF, aResponse,
                 FirstWords(apThis, 4), caller, CaptureStack());
    return RealWarningCallback(apThis, aResponse, a3, a4);
}
} // namespace

static TiltedPhoques::Initializer s_saveLoadHooks(
    []()
    {
#ifdef SKYRIMVR
        POINTER_SKYRIMSE(TSaveLoadManagerLoad, s_load, 34819, 34819);
        if (s_load.Get())
        {
            RealSaveLoadManagerLoad = s_load.Get();
            TP_HOOK(&RealSaveLoadManagerLoad, HookSaveLoadManagerLoad);
        }
        else
            spdlog::warn("SaveLoad: id 34819 not resolved, load diagnostic off");

        RealRequestCtor = reinterpret_cast<TRequestCtor*>(GameBase() + kRequestCtorRva);
        TP_HOOK(&RealRequestCtor, HookRequestCtor);
        RealWarningCallback = reinterpret_cast<TWarningCallback*>(GameBase() + kWarningCallbackRva);
        TP_HOOK(&RealWarningCallback, HookWarningCallback);
        spdlog::info("SaveLoad: request constructor and warning callback probes installed at SkyrimVR+0x{:x} and +0x{:x}", kRequestCtorRva, kWarningCallbackRva);
#endif
    });

void BGSSaveLoadManager::Save(SaveData* apData)
{
    apData->flags |= 4;

    const char* cSaveName = "";
    if (apData->saveName)
        cSaveName = apData->saveName;
}
