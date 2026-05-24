add_executable(fastauthd
    main.cpp
    server.cpp
    camera.cpp
    models.cpp
    pipeline.cpp
    enrollment_store.cpp
    handlers/test.cpp
    handlers/auth.cpp
    handlers/enroll.cpp
    handlers/list_remove.cpp
)
target_include_directories(fastauthd PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(fastauthd PRIVATE
    fastauth_common
    fastauth_proto
    onnxruntime::onnxruntime
    opencv_core
    opencv_imgproc
    opencv_calib3d
    PkgConfig::LIBSYSTEMD
    Threads::Threads
)
fastauth_apply_compile_options(fastauthd)
