add_executable(fastauthd
    main.cpp
    server.cpp
    handlers/test.cpp
    handlers/auth.cpp
    handlers/enroll.cpp
    handlers/list_remove.cpp
)
target_include_directories(fastauthd PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(fastauthd PRIVATE
    fastauth_common
    fastauth_proto
    PkgConfig::LIBSYSTEMD
    Threads::Threads
)
fastauth_apply_compile_options(fastauthd)
