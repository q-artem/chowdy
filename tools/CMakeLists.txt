# Milestone test-beds. Self-contained throwaway utilities — kept separate from
# the production daemon. Disable with -DCHOWDY_BUILD_TOOLS=OFF.

add_executable(chowdy-capture-test
    m1_capture_test.cpp
)
chowdy_apply_compile_options(chowdy-capture-test)

add_executable(chowdy-detect-test
    m2_detect_test.cpp
)
target_link_libraries(chowdy-detect-test PRIVATE
    onnxruntime::onnxruntime
    opencv_core opencv_imgproc opencv_imgcodecs opencv_calib3d
)
chowdy_apply_compile_options(chowdy-detect-test)

add_executable(chowdy-pipeline-test
    m3_pipeline_test.cpp
)
target_link_libraries(chowdy-pipeline-test PRIVATE
    onnxruntime::onnxruntime
    opencv_core opencv_imgproc opencv_calib3d
)
chowdy_apply_compile_options(chowdy-pipeline-test)
