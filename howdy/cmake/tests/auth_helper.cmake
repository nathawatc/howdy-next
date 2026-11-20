add_test(NAME native-auth-helper-help COMMAND howdy_auth_helper --help)
set(auth_helper_usage
 "Usage: $<TARGET_FILE:howdy_auth_helper> open <user>\n")
howdy_assert_binary_result(native-auth-helper-empty howdy_auth_helper 1
 "${auth_helper_usage}" "")
howdy_assert_binary_result(
 native-auth-helper-unknown-command howdy_auth_helper 1
 "${auth_helper_usage}" "" unknown alice)
howdy_assert_binary_result_nonroot(
 native-auth-helper-open-guard howdy_auth_helper 1 ""
 "howdy-auth-helper must be installed setuid root\n" open ../alice)
howdy_add_native_test(
 howdy_auth_helper_test
 native-auth-helper
 tests/auth_helper/auth_helper_test.cpp
 tests/auth_helper/auth_helper_diagnostics_test.cpp
 tests/auth_helper/auth_helper_descriptors_test.cpp
)
target_link_libraries(howdy_auth_helper_test PRIVATE howdy_auth_helper_core)
