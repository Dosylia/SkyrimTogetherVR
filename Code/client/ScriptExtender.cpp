
#include <ScriptExtender.h>
#include <TiltedOnlinePCH.h>
#include <VersionDb.h>

#ifdef SKYRIMVR
#include <tlhelp32.h>
#endif

namespace
{
#ifndef SKYRIMVR
constexpr wchar_t kScriptExtenderName[] = L"skse64";
#else
// SKSE VR ships as sksevr_1_4_15.dll.
constexpr wchar_t kScriptExtenderName[] = L"sksevr";
#endif

constexpr char kScriptExtenderEntrypoint[] = "StartSKSE";

constexpr size_t kScriptExtenderNameLength = sizeof(kScriptExtenderName) / sizeof(wchar_t) - 1;

// AE+ only
// Use this to raise the SKSE baseline
constexpr int kSKSEMinBuild = 20100;

HMODULE g_SKSEModuleHandle{nullptr};

struct FileVersion
{
    static constexpr uint8_t scVersionSize = 4;
    DWORD versions[scVersionSize];
};

int GetFileVersion(const std::filesystem::path& acFilePath, FileVersion& aVersion)
{
    const auto filename = acFilePath.c_str();

    DWORD dwHandle = 0, sz = GetFileVersionInfoSizeW(filename, &dwHandle);
    if (0 == sz)
    {
        return 1;
    }
    std::string buf(sz, '\0');
    if (!GetFileVersionInfoW(filename, dwHandle, sz, &buf[0]))
    {
        return 2;
    }
    VS_FIXEDFILEINFO* pvi;
    sz = sizeof(VS_FIXEDFILEINFO);
    if (!VerQueryValueA(&buf[0], "\\", reinterpret_cast<LPVOID*>(&pvi), reinterpret_cast<unsigned int*>(&sz)))
    {
        return 3;
    }

    aVersion.versions[0] = pvi->dwProductVersionMS >> 16;
    aVersion.versions[1] = pvi->dwFileVersionMS & 0xFFFF;
    aVersion.versions[2] = pvi->dwFileVersionLS >> 16;
    aVersion.versions[3] = pvi->dwFileVersionLS & 0xFFFF;

    return 0;
}

std::string GetSKSEStyleExeVersion()
{
    // make sure newer than anniversary!
    auto exeBuild = VersionDb::Get().GetLoadedVersionString();
    std::replace(exeBuild.begin(), exeBuild.end(), '.', '_');

    // Chop an empty patch component, so "1_6_323_0" becomes "1_6_323".
    //
    // This used to be find_last_of("_0"), which matches an underscore just as readily as a zero: a version with no
    // trailing ".0" -- "1.4.15" -> "1_4_15" -- matched the underscore at index 3 and was cut down to "1_", so the
    // module looked for was "sksevr_1_.dll" and SKSE was reported missing while it was plainly loaded. Seen's log
    // of 2026-09-25 says "SKSE VR is not loaded" twice; his crash dump from the same session lists
    // sksevr_1_4_15.dll. Only a real trailing "_0" is removed now.
    if (exeBuild.size() > 2 && exeBuild.compare(exeBuild.size() - 2, 2, "_0") == 0)
        exeBuild.erase(exeBuild.size() - 2);

    return exeBuild;
}

#ifdef SKYRIMVR
// Find sksevr_<anything>.dll among the modules the process has actually loaded.
//
// Building the name from the exe version was the wrong idea twice over. The first time the trimming was wrong
// ("1_4_15" cut to "1_"); that was fixed on 2026-09-25 and the warning came back the next morning, this time
// reading `Looked for 'sksevr_.dll', built from exe version ''` -- VersionDb has no loaded version string at this
// point, so there was nothing to build a name out of at all. Both failures share a cause: guessing at a filename
// the loader can simply be asked for.
//
// The consequence was not cosmetic. Everything gated on "is SKSE here" was off for the whole session while
// sksevr_1_4_15.dll was plainly in the process (Seen's crash dump lists it).
bool IsScriptExtenderModuleLoaded(std::wstring& aFoundName)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;

    std::wstring prefix(kScriptExtenderName);
    prefix += L'_';

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    bool found = false;
    if (Module32FirstW(snapshot, &entry))
    {
        do
        {
            std::wstring name(entry.szModule);
            std::wstring lowered = name;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });

            if (lowered.compare(0, prefix.size(), prefix) == 0 && lowered.size() > 4 && lowered.compare(lowered.size() - 4, 4, L".dll") == 0)
            {
                aFoundName = name;
                found = true;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return found;
}
#endif
} // namespace

#ifdef SKYRIMVR
// Read by the launcher's LdrLoadDll hook: EngineFixesVR is refused until SKSE starts, so it isn't
// loaded too early by the d3dx9_42 plugin preloader.
bool g_ScriptExtenderStarting = false;
#endif

bool IsScriptExtenderLoaded()
{
#ifdef SKYRIMVR
    // VR never loads SKSE itself (see main.cpp), so the handle above is always null here. Ask the loader which
    // modules are in the process rather than building a filename and hoping: see IsScriptExtenderModuleLoaded.
    std::wstring foundName;
    const bool found = IsScriptExtenderModuleLoaded(foundName);

    static bool s_said = false;
    if (!s_said)
    {
        s_said = true;
        if (found)
            spdlog::info("Script extender found: {}", std::string(foundName.begin(), foundName.end()));
        else
            spdlog::warn("Script extender not found: no sksevr_*.dll is loaded in this process.");
    }

    return found;
#else
    return g_SKSEModuleHandle;
#endif
}

void LoadScriptExtender()
{
    const auto exeVerson{GetSKSEStyleExeVersion()};

    // Get the path of the game, where the Script Extender dll resides
    const auto gameDir = std::filesystem::current_path();

    std::list<std::filesystem::path> dllMatches;
    for (const auto& dirEntry : std::filesystem::directory_iterator(gameDir))
    {
        const auto& path = dirEntry.path();
        if (path.extension() != L".dll")
            continue;

        auto fileName = path.filename().wstring();
        if (fileName.length() < kScriptExtenderNameLength)
            continue;

        if (fileName.substr(0, kScriptExtenderNameLength) == kScriptExtenderName)
        {
            dllMatches.push_back(path);
        }
    }

    // and before you ask, no, they dont expose it via file version info
    std::filesystem::path* needle = nullptr;
    for (auto& match : dllMatches)
    {
        auto fname = match.filename().string();
        auto ptr = &fname[kScriptExtenderNameLength + 1];
        // make extra sure!
        if (std::strncmp(ptr, exeVerson.c_str(), exeVerson.length()) == 0)
        {
            needle = &match;
            break;
        }
    }

    if (!needle)
    {
        spdlog::warn("No Script Extender matching game version {} found in {}", exeVerson, gameDir.string());
        return;
    }

    FileVersion fileVersion;
    if (GetFileVersion(*needle, fileVersion) != 0)
    {
        spdlog::error("Unable to verify Script Extender version");
        return;
    }

    auto skseVersion = fmt::format("v{}.{}.{}.{}", fileVersion.versions[0], fileVersion.versions[1], fileVersion.versions[2], fileVersion.versions[3]);

#ifdef SKYRIMVR
    // SKSE VR has no StartSKSE export and is older than kSKSEMinBuild: its DllMain initializes it,
    // so loading the DLL starts it (and its plugins, from inside LoadLibraryW).
    g_ScriptExtenderStarting = true;
    if (g_SKSEModuleHandle = LoadLibraryW(needle->c_str()))
        spdlog::info("SKSE VR {} loaded (initialized from DllMain). Messages without a colored [timestamp] prefix "
                     "come from the Script Extender and its plugins.",
                     skseVersion);
    else
        spdlog::error("Failed to load {} (error {})", needle->string(), GetLastError());
    return;
#endif

    // nice try.
    int SkseVCum = fileVersion.versions[0] * 1000000 + fileVersion.versions[1] * 10000 + fileVersion.versions[2] * 100 + fileVersion.versions[3];
    if (SkseVCum < kSKSEMinBuild)
    {
        spdlog::error("Pre anniversary Script Extender is unsupported");
        return;
    }

    if (g_SKSEModuleHandle = LoadLibraryW(needle->c_str()))
    {
        if (auto* pStartSKSE = reinterpret_cast<void (*)()>(GetProcAddress(g_SKSEModuleHandle, kScriptExtenderEntrypoint)))
        {
            spdlog::info(
                "Installing SKSE {} startup hooks... be aware that messages that start without a colored "
                "[timestamp] prefix are logs from the Script Extender and its loaded mods.",
                skseVersion);
            pStartSKSE();
            spdlog::info("SKSE startup hooks installed; initialization will continue during game startup");
        }
        else
            spdlog::warn("SKSE dll doesn't expose StartSKSE(), it may be outdated.");
    }
    else
    {
        spdlog::error("Failed to load {}! Check your privileges or re-download the Script Extender files.", needle->string());
    }
}
