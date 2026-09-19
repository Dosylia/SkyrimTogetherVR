set_xmakever("2.8.5")

-- If newer version of xmake, remove ccache until it actually works
if set_policy ~= nil then
    set_policy("build.ccache", false)
end

-- c code will use c99,
set_languages("c99", "cxx20")

if is_plat("windows") then
    add_cxflags("/bigobj")
    add_syslinks("kernel32")
    set_arch("x64")
end

if is_plat("linux") then
    add_cxflags("-fPIC")
end

set_warnings("all")
add_vectorexts("sse", "sse2", "sse3", "ssse3")
add_vectorexts("neon")

-- build configurations
add_rules("mode.debug", "mode.releasedbg", "mode.release")

if has_config("unitybuild") then
    add_rules("c.unity_build")
    add_rules("c++.unity_build", {batchsize = 12})
end

-- Local package overrides, must be registered before anything resolves packages.
-- Currently only carries a fork of the cef recipe; see xmake-repo/packages/c/cef.
add_repositories("tilted-local-repo xmake-repo")

-- direct dependencies version pinning
add_requires(
    "entt v3.10.0", 
    "recastnavigation v1.6.0", 
    "tiltedcore 0.2.8", 
    "cryptopp 8.9.0", 
    "spdlog v1.13.0", 
    "cpp-httplib 0.14.0",
    "gtest v1.14.0", 
    "mem 1.0.0", 
    "glm 0.9.9+8", 
    "zlib v1.3.1"
)
if is_plat("windows") then
    add_requires(
        "discord 3.2.1", 
        "imgui v1.89.7"
    )
end

-- dependencies' dependencies version pinning
add_requireconfs("*.mimalloc", { version = "2.2.4", override = true })
add_requireconfs("*.cmake", { version = "3.30.2", override = true })
add_requireconfs("*.openssl", { version = "1.1.1-w", override = true })
add_requireconfs("*.zlib", { version = "v1.3.1", override = true })
if is_plat("linux") then
    add_requireconfs("*.libcurl", { version = "8.7.1", override = true })
end

add_requireconfs("cpp-httplib", {configs = {ssl = true}})
--[[
add_requireconfs("magnum", { configs = { sdl2 = true }})
add_requireconfs("magnum-integration",  { configs = { imgui = true }})
add_requireconfs("magnum-integration.magnum",  { configs = { sdl2 = true }})
add_requireconfs("magnum-integration.imgui", { override = true })
--]]

-- The version the client and server compare on connect, as a compile define set when each target loads. A define is
-- part of xmake's dependency hash, so a changed version recompiles what embeds it. It was a generated header before,
-- and xmake did not rebuild for a change to that header made during the build (2026-09-18: a server DLL kept the
-- previous version, and later a whole build did). The value is computed once per xmake run and cached in _g.
on_load(function (target)
    if not _g.tilted_version then
        import("modules.version")
        local branch, commitHash, timestamp, describe = version()
        _g.tilted_version = { branch = branch, describe = describe }
    end
    target:add("defines", "BUILD_BRANCH=\"" .. _g.tilted_version.branch .. "\"", "BUILD_COMMIT=\"" .. _g.tilted_version.describe .. "\"")
end)

-- The version the client and server compare on connect, as a compile define set when each target loads. A define is
-- part of xmake's dependency hash, so a changed version recompiles what embeds it. It was a generated header before,
-- and xmake did not rebuild for a change to that header made during the build, so every version change needed two
-- builds (2026-09-18/19). Computed once per xmake run and cached in _g.
on_load(function (target)
    if not _g.tilted_version then
        import("modules.version")
        local branch, commitHash, timestamp, describe = version()
        _g.tilted_version = { branch = branch, describe = describe }
    end
    target:add("defines", 'BUILD_BRANCH="' .. _g.tilted_version.branch .. '"', 'BUILD_COMMIT="' .. _g.tilted_version.describe .. '"')
end)

before_build(function (target)
    import("modules.version")
    local branch, commitHash, timestamp, describe = version()
    bool_to_number={ [true]=1, [false]=0 }

    -- The version string for tooling (Tools\VR\make-release.ps1 names the zips after it). BuildInfo.h only holds
    -- fallbacks since the real value became a compile define, which left the release named "unknown-version".
    local versionPath = "build/BuildVersion.txt"
    if not os.exists(versionPath) or io.readfile(versionPath) ~= describe then
        io.writefile(versionPath, describe)
    end
    local contents = string.format([[
    #pragma once
    #define IS_MASTER %d
    #define IS_BRANCH_BETA %d
    #define IS_BRANCH_PREREL %d
    ]],
    bool_to_number[branch == "master"],
    bool_to_number[branch == "bluedove"],
    bool_to_number[branch == "prerel"])

    -- fix always-compiles problem by updating the file only if content has changed.
    local filepath = "build/BranchInfo.h"
    local old_content = nil
    if os.exists(filepath) then
        old_content = io.readfile(filepath)
    end
    if old_content ~= contents then
        print("Updating file:", filepath)
        io.writefile(filepath, contents)
    end

    -- BuildInfo.h is still included by the sources; it only carries fallbacks now, the values are the defines above.
    local buildInfoPath = "build/BuildInfo.h"
    local buildInfo = [[
#pragma once
// Generated by xmake (root xmake.lua). BUILD_BRANCH and BUILD_COMMIT are compile defines; these are fallbacks.
#ifndef BUILD_BRANCH
#define BUILD_BRANCH "unknown-branch"
#endif
#ifndef BUILD_COMMIT
#define BUILD_COMMIT "unknown-version"
#endif
]]
    if not os.exists(buildInfoPath) or io.readfile(buildInfoPath) ~= buildInfo then
        io.writefile(buildInfoPath, buildInfo)
    end
end)

if is_mode("debug") then
    add_defines("DEBUG")
end

if is_plat("windows") then
    add_defines("NOMINMAX")
end

-- add projects
includes("Libraries")
includes("Code")
