howdy_add_native_test(
	howdy_file_security_test
	native-file-security
	tests/support/file_security_test.cpp
)

howdy_add_native_test(
	howdy_fd_io_test
	native-fd-io
	tests/support/fd_io_test.cpp
)

howdy_add_native_test(
	howdy_user_names_test
	native-user-names
	tests/support/user_names_test.cpp
)

howdy_add_native_test(
	howdy_invoking_user_env_test
	native-invoking-user-env
	tests/support/invoking_user_env_test.cpp
)
