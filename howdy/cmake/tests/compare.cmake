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

add_executable(howdy_compare_args_test tests/compare/compare_args_test.cpp)
howdy_configure_native_test_target(howdy_compare_args_test)
target_link_libraries(howdy_compare_args_test PRIVATE howdy_compare_args)
add_test(NAME native-compare-args COMMAND howdy_compare_args_test)

add_executable(howdy_compare_logic_test tests/compare/compare_logic_test.cpp)
howdy_configure_native_test_target(howdy_compare_logic_test)
target_link_libraries(
	howdy_compare_logic_test PRIVATE howdy_compare_logic howdy_opencv
)
add_test(NAME native-compare-logic COMMAND howdy_compare_logic_test)

add_executable(
	howdy_frame_processing_test tests/vision/frame_processing_test.cpp
)
howdy_configure_native_test_target(howdy_frame_processing_test)
target_link_libraries(
	howdy_frame_processing_test
	PRIVATE howdy_frame_processing howdy_compare_logic howdy_opencv
)
add_test(NAME native-frame-processing COMMAND howdy_frame_processing_test)

add_executable(
	howdy_compare_engine_test
	tests/compare/compare_engine_test.cpp
	tests/compare/compare_engine_frame_test.cpp
	tests/compare/compare_engine_inference_test.cpp
)
howdy_configure_native_test_target(howdy_compare_engine_test)
target_link_libraries(
	howdy_compare_engine_test
	PRIVATE
		howdy_compare_engine
		howdy_frame_processing
		howdy_compare_logic
		howdy_opencv
)
add_test(NAME native-compare-engine COMMAND howdy_compare_engine_test)

add_executable(
	howdy_compare_capture_session_test
	tests/compare/compare_capture_session_test.cpp
)
howdy_configure_native_test_target(howdy_compare_capture_session_test)
target_link_libraries(
	howdy_compare_capture_session_test
	PRIVATE howdy_compare_capture_session
)
add_test(
	NAME native-compare-capture-session
	COMMAND howdy_compare_capture_session_test
)

add_executable(
	howdy_compare_privileges_test
	tests/compare/compare_privileges_test.cpp
	tests/compare/compare_privileges_non_root_test.cpp
	tests/compare/compare_privileges_privileged_test.cpp
	tests/compare/compare_privileges_fatal_test.cpp
)
howdy_configure_native_test_target(howdy_compare_privileges_test)
target_link_libraries(
	howdy_compare_privileges_test PRIVATE howdy_compare_privileges
)
add_test(NAME native-compare-privileges COMMAND howdy_compare_privileges_test)

add_executable(
	howdy_compare_processing_test tests/compare/compare_processing_test.cpp
)
howdy_configure_native_test_target(howdy_compare_processing_test)
target_link_libraries(
	howdy_compare_processing_test PRIVATE howdy_compare_processing
)
add_test(NAME native-compare-processing COMMAND howdy_compare_processing_test)

add_executable(
	howdy_compare_sandbox_test tests/compare/compare_sandbox_test.cpp
)
howdy_configure_native_test_target(howdy_compare_sandbox_test)
target_link_libraries(howdy_compare_sandbox_test PRIVATE howdy_compare_sandbox)
add_test(NAME native-compare-sandbox COMMAND howdy_compare_sandbox_test)
