add_executable(chowdyd
    main.cpp
    server.cpp
    camera.cpp
    models.cpp
    pipeline.cpp
    enrollment_store.cpp
    enroll_session.cpp
    handlers/test.cpp
    handlers/auth.cpp
    handlers/enroll.cpp
    handlers/list_remove.cpp
)
target_include_directories(chowdyd PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(chowdyd PRIVATE
    chowdy_common
    chowdy_proto
    onnxruntime::onnxruntime
    opencv_core
    opencv_imgproc
    ${CHOWDY_OPENCV_AFFINE_LIB}
    PkgConfig::LIBSYSTEMD
    Threads::Threads
)
chowdy_apply_compile_options(chowdyd)
