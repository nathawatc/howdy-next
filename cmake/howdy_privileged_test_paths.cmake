function(howdy_validate_privileged_test_prefix prefix)
	if(NOT "${prefix}" MATCHES "^/opt/howdy-installed-pam-e2e-[A-Za-z0-9._-]+$")
		message(
			FATAL_ERROR
				"Privileged tests require an isolated CMAKE_INSTALL_PREFIX "
				"matching /opt/howdy-installed-pam-e2e-*"
		)
	endif()
endfunction()

function(howdy_validate_privileged_test_paths)
	set(one_value_args
		CMAKE_INSTALL_PREFIX
		CMAKE_INSTALL_BINDIR
		CMAKE_INSTALL_DATADIR
		CMAKE_INSTALL_LOCALEDIR
		CMAKE_INSTALL_MANDIR
		HOWDY_PAM_DIR
		HOWDY_CONFIG_DIR
		HOWDY_MODELS_DIR
		HOWDY_USER_MODELS_DIR
		HOWDY_LOG_PATH
		HOWDY_HELPER_DIR
		HOWDY_AUTH_HELPER_PATH
		HOWDY_LICENSES_INSTALL_DIR
	)
	cmake_parse_arguments(PARSE_ARGV 0 howdy_privileged_test_paths ""
                       "${one_value_args}" "")

	howdy_validate_privileged_test_prefix(
		"${howdy_privileged_test_paths_CMAKE_INSTALL_PREFIX}"
	)
	set(howdy_licenses_install_path
		"${howdy_privileged_test_paths_HOWDY_LICENSES_INSTALL_DIR}"
	)
	set(howdy_install_prefix
		"${howdy_privileged_test_paths_CMAKE_INSTALL_PREFIX}"
	)
	cmake_path(
		ABSOLUTE_PATH howdy_licenses_install_path
		BASE_DIRECTORY "${howdy_privileged_test_paths_CMAKE_INSTALL_PREFIX}"
		NORMALIZE
	)
	foreach(
		isolated_path
		IN ITEMS
			"${howdy_privileged_test_paths_CMAKE_INSTALL_BINDIR}"
			"${howdy_privileged_test_paths_CMAKE_INSTALL_DATADIR}"
			"${howdy_privileged_test_paths_CMAKE_INSTALL_LOCALEDIR}"
			"${howdy_privileged_test_paths_CMAKE_INSTALL_MANDIR}"
			"${howdy_privileged_test_paths_HOWDY_PAM_DIR}"
			"${howdy_privileged_test_paths_HOWDY_CONFIG_DIR}"
			"${howdy_privileged_test_paths_HOWDY_MODELS_DIR}"
			"${howdy_privileged_test_paths_HOWDY_USER_MODELS_DIR}"
			"${howdy_privileged_test_paths_HOWDY_LOG_PATH}"
			"${howdy_privileged_test_paths_HOWDY_HELPER_DIR}"
			"${howdy_privileged_test_paths_HOWDY_AUTH_HELPER_PATH}"
			"${howdy_licenses_install_path}"
	)
		cmake_path(
			IS_PREFIX howdy_install_prefix "${isolated_path}"
			NORMALIZE path_is_isolated
		)
		if(NOT path_is_isolated)
			message(
				FATAL_ERROR
				"Privileged-test path escapes isolated prefix: ${isolated_path}"
			)
		endif()
	endforeach()
endfunction()
