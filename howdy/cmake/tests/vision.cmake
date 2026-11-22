howdy_add_native_test(
 howdy_face_matching_test
 native-face-matching
 tests/vision/face_matching_test.cpp
)

howdy_add_native_test(
 howdy_face_detection_test
 native-face-detection
 tests/vision/face_detection_test.cpp
)
target_link_libraries(howdy_face_detection_test PRIVATE howdy_vision_core)

howdy_add_native_test(
 howdy_face_encoding_test
 native-face-encoding
 tests/vision/face_encoding_test.cpp
)
target_link_libraries(howdy_face_encoding_test PRIVATE howdy_vision_core)

howdy_add_native_test(
 howdy_face_model_test
 native-face-model
 tests/vision/face_model_test.cpp
 tests/vision/face_model_environment_test.cpp
)
target_link_libraries(
 howdy_face_model_test
 PRIVATE
  howdy_face_model
  howdy_runtime_paths
)

howdy_add_native_test(
 howdy_video_capture_test
 native-video-capture
 tests/vision/video_capture_test.cpp
)
target_link_libraries(howdy_video_capture_test PRIVATE howdy_vision_core)

howdy_add_native_test(
 howdy_frame_processing_test
 native-frame-processing
 tests/vision/frame_processing_test.cpp
)
target_link_libraries(
 howdy_frame_processing_test
 PRIVATE howdy_frame_processing howdy_opencv
)

howdy_add_native_test(
 howdy_preview_engine_test
 native-preview-engine
 tests/vision/preview_engine_test.cpp
)
target_link_libraries(
 howdy_preview_engine_test
 PRIVATE howdy_preview_engine
)
