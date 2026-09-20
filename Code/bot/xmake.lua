-- STBot: a headless player. It connects to a server like the game client does, assigns a player character cloned
-- from the host's appearance, and drives it from a script (walk, equip, die, respawn, reconnect). It links only the
-- message encoding and the transport, so it never touches the game client or the server; see VR_TODO.md section 6.1.
target("STBot")
    set_kind("binary")
    set_group("Tests")
    add_includedirs(".", "../encoding")
    add_headerfiles("*.h")
    add_files("*.cpp")
    add_deps("SkyrimEncoding", "TiltedConnect")
    add_packages(
        "tiltedcore",
        "hopscotch-map",
        "mimalloc",
        "glm",
        "spdlog",
        "gamenetworkingsockets",
        "libuv",
        "snappy")
    add_defines("STEAMNETWORKINGSOCKETS_STATIC_LINK", "NOMINMAX")
    if is_plat("windows") then
        add_syslinks("ws2_32", "iphlpapi", "crypt32", "bcrypt", "userenv", "psapi")
    end
