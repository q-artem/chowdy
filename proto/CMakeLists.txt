# proto/ is header-only: IPC message types and JSON (de)serialization.
add_library(chowdy_proto INTERFACE)
target_include_directories(chowdy_proto INTERFACE
    ${CMAKE_SOURCE_DIR}
)
target_link_libraries(chowdy_proto INTERFACE
    nlohmann_json::nlohmann_json
)
chowdy_apply_compile_options(chowdy_proto)
