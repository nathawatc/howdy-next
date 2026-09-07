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
howdy_add_native_test(
	howdy_auth_helper_test
	native-auth-helper
	tests/auth_helper/auth_helper_test.cpp
	tests/auth_helper/auth_helper_path_test.cpp
	tests/auth_helper/auth_helper_staging_test.cpp
)
target_link_libraries(howdy_auth_helper_test PRIVATE howdy_auth_helper_core)
