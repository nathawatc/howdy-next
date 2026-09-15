add_test(NAME native-compare-help COMMAND howdy_compare --help)
add_test(NAME native-compare-unknown-arg COMMAND howdy_compare --unknown)
howdy_expect_test_failure(native-compare-unknown-arg)
add_test(NAME native-compare-missing-user COMMAND howdy_compare)
howdy_expect_test_failure(native-compare-missing-user)
add_test(NAME native-compare-no-model COMMAND howdy_compare coverage-user)
set(
	compare_test_models_empty
	"${CMAKE_CURRENT_BINARY_DIR}/compare-test-models-empty"
)
set_tests_properties(
	native-compare-no-model
	PROPERTIES
		WILL_FAIL TRUE
		ENVIRONMENT "HOWDY_USER_MODELS_DIR=${compare_test_models_empty}"
)

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
