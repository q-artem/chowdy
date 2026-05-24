add_library(chowdy_common STATIC
    ipc.cpp
    peer_cred.cpp
    logging.cpp
    encoding.cpp
    config.cpp
)
target_include_directories(chowdy_common PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(chowdy_common
    PUBLIC
        chowdy_proto
        nlohmann_json::nlohmann_json
        PkgConfig::LIBSYSTEMD
        Threads::Threads
)
find_package(tomlplusplus QUIET)
if(tomlplusplus_FOUND)
    target_link_libraries(chowdy_common PUBLIC tomlplusplus::tomlplusplus)
else()
    # Arch ships toml++ as a header-only library under /usr/include/toml++.
    # When the CMake config isn't installed, the header is still on the
    # default include path — nothing to do.
endif()
chowdy_apply_compile_options(chowdy_common)
