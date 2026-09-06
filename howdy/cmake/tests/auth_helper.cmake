add_test(NAME native-auth-helper-help COMMAND howdy_auth_helper --help)
add_test(NAME native-auth-helper-empty COMMAND howdy_auth_helper)
howdy_expect_test_failure(native-auth-helper-empty)
add_test(
	NAME native-auth-helper-unknown-command
	COMMAND howdy_auth_helper unknown alice
)
howdy_expect_test_failure(native-auth-helper-unknown-command)
add_test(
	NAME native-auth-helper-prepare-guard
	COMMAND howdy_auth_helper prepare ../alice
)
howdy_expect_test_failure(native-auth-helper-prepare-guard)
add_executable(
	howdy_auth_helper_test
	tests/auth_helper/auth_helper_test.cpp
	tests/auth_helper/auth_helper_path_test.cpp
	tests/auth_helper/auth_helper_staging_test.cpp
)
howdy_configure_native_test_target(howdy_auth_helper_test)
target_link_libraries(howdy_auth_helper_test PRIVATE howdy_auth_helper_core)
add_test(NAME native-auth-helper COMMAND howdy_auth_helper_test)
