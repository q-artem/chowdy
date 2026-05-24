# proto/ is header-only: IPC message types and JSON (de)serialization.
add_library(fastauth_proto INTERFACE)
target_include_directories(fastauth_proto INTERFACE
    ${CMAKE_SOURCE_DIR}
)
target_link_libraries(fastauth_proto INTERFACE
    nlohmann_json::nlohmann_json
)
fastauth_apply_compile_options(fastauth_proto)
