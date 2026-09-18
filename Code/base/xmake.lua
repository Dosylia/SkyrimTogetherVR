
target("BaseLib")
    set_kind("static")
    set_group("common")
    add_includedirs(".", "../", "../../build", {public = true})
    add_headerfiles("**.h")
    add_files("**.cpp")
    add_packages(
        "tiltedcore",
        "hopscotch-map", 
        "gtest",
        "spdlog")
