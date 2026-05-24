# Milestone test-beds. Self-contained throwaway utilities — kept separate from
# the production daemon. Disable with -DFASTAUTH_BUILD_TOOLS=OFF.

add_executable(fastauth-capture-test
    m1_capture_test.cpp
)
fastauth_apply_compile_options(fastauth-capture-test)

add_executable(fastauth-detect-test
    m2_detect_test.cpp
)
target_link_libraries(fastauth-detect-test PRIVATE
    onnxruntime::onnxruntime
    opencv_core opencv_imgproc opencv_imgcodecs opencv_calib3d
)
fastauth_apply_compile_options(fastauth-detect-test)

add_executable(fastauth-pipeline-test
    m3_pipeline_test.cpp
)
target_link_libraries(fastauth-pipeline-test PRIVATE
    onnxruntime::onnxruntime
    opencv_core opencv_imgproc opencv_calib3d
)
fastauth_apply_compile_options(fastauth-pipeline-test)
