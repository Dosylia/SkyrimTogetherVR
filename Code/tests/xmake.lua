
target("TPTests")
    set_kind("binary")
    set_group("Tests")
    add_includedirs(
        ".", "../encoding", "../client")
    add_headerfiles("**.h")
    add_files("*.cpp")
    add_deps("SkyrimEncoding")
    if is_plat("windows") then
        -- SmallDump.h (client) writes minidumps.
        add_syslinks("dbghelp")
    else
        -- Windows minidumps: on Linux this file broke the server's CI build from 2026-10-07 (c5d3236e).
        remove_files("smalldump.cpp")
    end
    add_packages(
        "tiltedcore",
        "hopscotch-map",
        "catch2",
        "mimalloc",
        "glm")
