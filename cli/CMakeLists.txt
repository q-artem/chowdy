add_executable(chowdy-cli
    main.cpp
)
target_include_directories(chowdy-cli PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(chowdy-cli PRIVATE
    chowdy_common
    chowdy_proto
)
chowdy_apply_compile_options(chowdy-cli)
