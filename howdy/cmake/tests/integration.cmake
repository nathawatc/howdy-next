set(howdy_integration_dir "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration")
add_test(NAME native-howdy-help COMMAND howdy --help)
add_test(NAME native-howdy-version COMMAND howdy version)
add_test(NAME native-howdy-nonroot-path COMMAND howdy -U coverage-user list)
howdy_expect_test_failure(native-howdy-nonroot-path)
add_test(NAME native-howdy-empty COMMAND howdy)
add_test(
	NAME native-howdy-bash-completion
	COMMAND
		bash
		"${howdy_integration_dir}/bash_completion_test.sh"
		"$<TARGET_FILE:howdy>"
		"${CMAKE_CURRENT_SOURCE_DIR}/completions/howdy"
)

add_test(
	NAME native-howdy-install-layout
	COMMAND
		sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/install_layout_test.sh"
		"${PROJECT_BINARY_DIR}"
		"${CMAKE_INSTALL_PREFIX}"
		"${CMAKE_INSTALL_BINDIR}"
		"${CMAKE_INSTALL_LIBEXECDIR}"
		"${CMAKE_INSTALL_DATADIR}"
		"${CMAKE_INSTALL_LOCALEDIR}"
		"${CMAKE_INSTALL_MANDIR}"
		"${HOWDY_HELPER_DIR}"
		"${HOWDY_PAM_DIR}"
		"${HOWDY_CONFIG_DIR}"
		"${HOWDY_CONFIG_PATH}"
		"${HOWDY_MODELS_DIR}"
		"${HOWDY_USER_MODELS_DIR}"
		"${HOWDY_LICENSES_INSTALL_DIR}"
)
add_test(
	NAME native-howdy-privileged-install-paths
	COMMAND
		sh
		"${howdy_integration_dir}/privileged_install_paths_test.sh"
		"${PROJECT_SOURCE_DIR}"
		"${PROJECT_BINARY_DIR}/privileged-install-path-cases"
)
if(NOT CMAKE_CROSSCOMPILING)
	add_test(
		NAME native-howdy-docs-generator
		COMMAND
			sh
			"${howdy_integration_dir}/docs_generator_test.sh"
			"$<TARGET_FILE:howdy_docs_generator>"
			"${PROJECT_BINARY_DIR}/docs-generator-test"
	)
endif()
add_test(
	NAME native-howdy-docs
	COMMAND
		sh
		"${howdy_integration_dir}/docs_test.sh"
		"${PROJECT_BINARY_DIR}"
		"${PROJECT_SOURCE_DIR}"
		"${HOWDY_MAN1_PATH}"
		"${HOWDY_MAN5_PATH}"
		"${HOWDY_MAN8_PATH}"
		"${HOWDY_CONFIG_DIR}"
		"${HOWDY_CONFIG_PATH}"
		"${HOWDY_MODELS_DIR}"
		"${HOWDY_USER_MODELS_DIR}"
		"${HOWDY_AUTH_HELPER_PATH}"
)
