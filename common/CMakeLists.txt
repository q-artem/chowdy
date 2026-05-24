add_library(fastauth_common STATIC
    ipc.cpp
    peer_cred.cpp
    logging.cpp
)
target_include_directories(fastauth_common PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(fastauth_common
    PUBLIC
        fastauth_proto
        nlohmann_json::nlohmann_json
        PkgConfig::LIBSYSTEMD
        Threads::Threads
)
fastauth_apply_compile_options(fastauth_common)
