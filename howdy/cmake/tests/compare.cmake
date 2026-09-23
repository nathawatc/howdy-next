add_test(NAME native-compare-help COMMAND howdy_compare --help)
add_test(NAME native-compare-unknown-arg COMMAND howdy_compare --unknown)
howdy_expect_test_failure(native-compare-unknown-arg)
add_test(NAME native-compare-missing-user COMMAND howdy_compare)
howdy_expect_test_failure(native-compare-missing-user)
howdy_add_native_test(
	howdy_compare_entrypoint_test
	native-compare-no-model
	tests/compare/compare_entrypoint_test.cpp
)
set(
	compare_no_model_root
	"${CMAKE_CURRENT_BINARY_DIR}/compare-no-model-test"
)
target_compile_definitions(
	howdy_compare_entrypoint_test
	PRIVATE
		"HOWDY_PACKAGED_CONFIG_PATH=\"${HOWDY_PACKAGED_CONFIG_PATH}\""
		"HOWDY_COMPARE_NO_MODEL_ROOT=\"${compare_no_model_root}\""
)
target_link_libraries(howdy_compare_entrypoint_test PRIVATE
                      howdy_compare_entrypoint howdy_opencv)
add_dependencies(howdy_compare_entrypoint_test howdy_packaged_config)

howdy_add_native_test(
	howdy_compare_args_test
	native-compare-args
	tests/compare/compare_args_test.cpp
)
target_link_libraries(howdy_compare_args_test PRIVATE howdy_compare_args)

howdy_add_native_test(
	howdy_compare_logic_test
	native-compare-logic
	tests/compare/compare_logic_test.cpp
)
target_link_libraries(
	howdy_compare_logic_test PRIVATE howdy_compare_logic howdy_opencv
)

howdy_add_native_test(
	howdy_frame_processing_test
	native-frame-processing
	tests/vision/frame_processing_test.cpp
)
target_link_libraries(
	howdy_frame_processing_test
	PRIVATE howdy_frame_processing howdy_compare_logic howdy_opencv
)

howdy_add_native_test(
	howdy_compare_engine_test
	native-compare-engine
	tests/compare/compare_engine_test.cpp
	tests/compare/compare_engine_frame_test.cpp
	tests/compare/compare_engine_inference_test.cpp
)
target_link_libraries(
	howdy_compare_engine_test
	PRIVATE
		howdy_compare_engine
		howdy_opencv
)

howdy_add_native_test(
	howdy_compare_capture_session_test
	native-compare-capture-session
	tests/compare/compare_capture_session_test.cpp
)
target_link_libraries(
	howdy_compare_capture_session_test
	PRIVATE howdy_compare_capture_session
)

howdy_add_native_test(
	howdy_compare_privileges_test
	native-compare-privileges
	tests/compare/compare_privileges_test.cpp
	tests/compare/compare_privileges_non_root_test.cpp
	tests/compare/compare_privileges_privileged_test.cpp
	tests/compare/compare_privileges_fatal_test.cpp
)
target_link_libraries(
	howdy_compare_privileges_test PRIVATE howdy_compare_privileges
)

howdy_add_native_test(
	howdy_compare_processing_test
	native-compare-processing
	tests/compare/compare_processing_test.cpp
)
target_link_libraries(
	howdy_compare_processing_test PRIVATE howdy_compare_processing
)

howdy_add_native_test(
	howdy_compare_sandbox_test
	native-compare-sandbox
	tests/compare/compare_sandbox_test.cpp
)
target_link_libraries(howdy_compare_sandbox_test PRIVATE howdy_compare_sandbox)
