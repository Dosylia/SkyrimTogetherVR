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
#endif
    });

void BGSSaveLoadManager::Save(SaveData* apData)
{
    apData->flags |= 4;

    const char* cSaveName = "";
    if (apData->saveName)
        cSaveName = apData->saveName;
}
