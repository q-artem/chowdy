add_executable(fastauth-cli
    main.cpp
)
target_include_directories(fastauth-cli PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(fastauth-cli PRIVATE
    fastauth_common
    fastauth_proto
)
fastauth_apply_compile_options(fastauth-cli)
