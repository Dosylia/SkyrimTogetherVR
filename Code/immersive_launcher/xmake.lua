local function istable(t) return type(t) == 'table' end

local function build_launcher()
    set_kind("binary")
    set_group("Client")
    set_symbols("debug", "hidden")

    add_ldflags(
        "/FORCE:MULTIPLE",
        "/IGNORE:4254,4006",
        "/DYNAMICBASE:NO",
        "/SAFESEH:NO",
        "/LARGEADDRESSAWARE",
        "/INCREMENTAL:NO",
        "/LAST:.zdata",
        "/SUBSYSTEM:WINDOWS",
        "/ENTRY:mainCRTStartup", { force = true })
    add_includedirs(
        ".",
        "../",
        "../../Libraries/")
    add_headerfiles("**.h")
    add_files(
        "**.cpp",
        "launcher.rc")
    add_deps(
        "ImmersiveElf",
        "TiltedReverse",
        "TiltedHooks",
        "TiltedUi",
        "ImGuiImpl",
        "CommonLib")
    add_links("ntdll_x64")
    add_linkdirs(".")
    add_syslinks(
        "user32",
        "shell32",
        "comdlg32",
        "bcrypt",
        "ole32",
        "dxgi",
        "d3d11",
        "gdi32",
        "SetupAPI",
        "Powrprof",
        "Cfgmgr32",
        "Propsys",
        "delayimp")

    add_packages(
        "tiltedcore",
        "spdlog",
        "minhook",
        "hopscotch-map",
        "cryptopp",
        "glm",
        "cef",
        "mem")
end

target("SkyrimImmersiveLauncher")
    set_basename("SkyrimTogether")
    add_defines("TARGET_PREFIX=\"st\"")
    add_deps("SkyrimTogetherClient")
    add_ldflags("/WHOLEARCHIVE:SkyrimTogetherClient", { force = true })
    build_launcher()

target("SkyrimImmersiveLauncherVR")
    set_basename("urSovngarde")
    add_defines("TARGET_PREFIX=\"st\"")
    add_defines("SKYRIMVR")
    add_deps("SkyrimTogetherClientVR")
    add_ldflags("/WHOLEARCHIVE:SkyrimTogetherClientVR", { force = true })
    -- A linker map, so a crash address can be turned back into a function name.
    --
    -- The launcher replaces the game's executable, so a coredump shows one 89.8 MB module holding both the
    -- game's code and ours and the module name answers nothing. dbghelp against the PDB would, but it refused
    -- to load symbols on 2026-09-27 and a whole evening went on guessing which code a stack belonged to. A map
    -- is a sorted list of addresses and names in a text file: no debugger, no PDB matching, twenty lines of
    -- Python to read. Keep it next to the build it came from.
    add_ldflags("/MAP", { force = true })
    build_launcher()
