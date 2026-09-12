# Exercise security fixtures below an unrelated writable ancestor on every run,
# including builds outside /tmp. Each suite owns its fixture boundary below.
set(howdy_security_test_workdir
    "${CMAKE_CURRENT_BINARY_DIR}/world-writable-test-work")
file(MAKE_DIRECTORY "${howdy_security_test_workdir}")
file(
	CHMOD "${howdy_security_test_workdir}"
	PERMISSIONS
		OWNER_READ OWNER_WRITE OWNER_EXECUTE
		GROUP_READ GROUP_WRITE GROUP_EXECUTE
		WORLD_READ WORLD_WRITE WORLD_EXECUTE
)

howdy_add_native_test(
	howdy_file_security_test
	native-file-security
	tests/support/file_security_test.cpp
)

set_tests_properties(
	native-auth-helper
	native-config-utils
	native-runtime-config-load
	native-runtime-config
	native-model-file
	native-user-models
	native-clear-cli
	native-config-cli
	native-disable-cli
	native-download-models
	native-snapshot-writer
	native-file-security
	PROPERTIES WORKING_DIRECTORY "${howdy_security_test_workdir}"
)

set_tests_properties(
	native-runtime-config-load
	native-runtime-config
	PROPERTIES ENVIRONMENT "TMPDIR=${howdy_security_test_workdir}"
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
