add_test(NAME native-compare-help COMMAND howdy_compare --help)
set_property(
 TEST native-compare-help
 PROPERTY
  ENVIRONMENT_MODIFICATION
  "HOWDY_COMPARE_VERDICT_FD=unset:"
  "HOWDY_COMPARE_CONFIG_FD=unset:"
  "HOWDY_COMPARE_MODEL_FD=unset:"
)
howdy_assert_binary_result(native-compare-unknown-arg howdy_compare 12 ""
 "Unknown argument: --unknown\n" --unknown)
howdy_assert_binary_result(native-compare-pam-help-user howdy_compare 12 ""
 "Unknown argument: --help\n" --help)
set_tests_properties(
 native-compare-pam-help-user
 PROPERTIES ENVIRONMENT "HOWDY_COMPARE_VERDICT_FD=3"
)
howdy_assert_binary_result(native-compare-trailing-help howdy_compare 12 ""
 "Unknown argument: --help\n" alice --help)
howdy_assert_binary_result(native-compare-missing-user howdy_compare 12 "" "")
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

set(compare_success_root "${CMAKE_CURRENT_BINARY_DIR}/compare-success-test")
howdy_add_native_test(
 howdy_compare_entrypoint_success_test
 native-compare-entrypoint-success
 tests/compare/compare_entrypoint_success_test.cpp
)
target_compile_definitions(
 howdy_compare_entrypoint_success_test
 PRIVATE
  "HOWDY_PACKAGED_CONFIG_PATH=\"${HOWDY_PACKAGED_CONFIG_PATH}\""
  "HOWDY_COMPARE_SUCCESS_ROOT=\"${compare_success_root}\""
)
target_link_libraries(howdy_compare_entrypoint_success_test PRIVATE
                      howdy_compare_entrypoint howdy_opencv)
add_dependencies(howdy_compare_entrypoint_success_test howdy_packaged_config)
# The entrypoint applies the 2.5 GiB address-space limit in this process. Pin the same threads the
# PAM module passes to compare so an OpenMP-built OpenBLAS does not reserve the limit at load.
set_tests_properties(
 native-compare-entrypoint-success
 PROPERTIES ENVIRONMENT "OPENBLAS_NUM_THREADS=1;OMP_NUM_THREADS=1"
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
 tests/compare/compare_sandbox_limit_test.cpp
 tests/compare/compare_sandbox_tracing_test.cpp
 tests/compare/compare_sandbox_seccomp_test.cpp
 tests/compare/compare_sandbox_seccomp_exec_test.cpp
)
target_link_libraries(howdy_compare_sandbox_test PRIVATE howdy_compare_sandbox)

# Big-endian and other targets without a filter, on any build: the sandbox is compiled in with the
# filter left out.
howdy_add_native_test(
 howdy_compare_sandbox_unsupported_arch_test
 native-compare-sandbox-unsupported-arch
 tests/compare/compare_sandbox_test.cpp
 tests/compare/compare_sandbox_limit_test.cpp
 tests/compare/compare_sandbox_tracing_test.cpp
 tests/compare/compare_sandbox_seccomp_test.cpp
 tests/compare/compare_sandbox_seccomp_exec_test.cpp
 src/compare/sandbox.cpp
 src/compare/sandbox/seccomp.cpp
)
target_compile_definitions(
 howdy_compare_sandbox_unsupported_arch_test
 PRIVATE HOWDY_SECCOMP_UNSUPPORTED_ARCH=1
)
set_target_properties(
 howdy_compare_sandbox_unsupported_arch_test
 PROPERTIES HOWDY_SKIP_COMPILE_COMMANDS ON
)
