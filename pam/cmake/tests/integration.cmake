set(pam_integration_dir "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration")
add_test(
	NAME pam-installed-setuid-path-validation
	COMMAND
		bash
		"${pam_integration_dir}/installed_pam_setuid_e2e.sh"
		--validate-paths
)
howdy_set_test_timeouts()

if(HOWDY_ENABLE_PRIVILEGED_TESTS)
	if(HOWDY_HAVE_PAM_START_CONFDIR)
		add_executable(
			pam_installed_e2e_driver
			tests/integration/installed_pam_driver.cpp
		)
		howdy_configure_pam_target(pam_installed_e2e_driver)
		target_link_libraries(pam_installed_e2e_driver PRIVATE PAM::PAM)
		add_executable(
			pam_installed_runtime_e2e_driver
			tests/integration/installed_runtime_e2e_driver.cpp
		)
		howdy_configure_pam_target(pam_installed_runtime_e2e_driver)
		target_link_libraries(
			pam_installed_runtime_e2e_driver
			PRIVATE pam_runtime howdy_user_models howdy_user_model_readiness
		)
		add_custom_target(
			howdy-installed-e2e-artifacts
			DEPENDS
				howdy
				howdy_compare
				howdy_auth_helper
				pam_howdy
				howdy_packaged_config
				howdy_docs
				howdy-gmo
				pam_installed_e2e_driver
				pam_installed_runtime_e2e_driver
		)
		add_test(
			NAME installed-pam-setuid-e2e
			COMMAND
				bash
				"${pam_integration_dir}/installed_pam_setuid_e2e.sh"
				"${PROJECT_BINARY_DIR}"
				"${CMAKE_INSTALL_PREFIX}"
				"${HOWDY_PAM_DIR}"
				"${HOWDY_HELPER_DIR}"
				"${HOWDY_CONFIG_DIR}"
				"${HOWDY_USER_MODELS_DIR}"
				"$<TARGET_FILE:pam_installed_e2e_driver>"
				"$<TARGET_FILE:pam_installed_runtime_e2e_driver>"
		)
		add_test(
			NAME installed-pam-setuid-install-environment
			COMMAND
				sh
				"${pam_integration_dir}/installed_pam_install_environment_test.sh"
				"${pam_integration_dir}/installed_pam_setuid_e2e.sh"
				"${PROJECT_BINARY_DIR}"
				"${CMAKE_INSTALL_PREFIX}"
				"${HOWDY_PAM_DIR}"
				"${HOWDY_HELPER_DIR}"
				"${HOWDY_CONFIG_DIR}"
				"${HOWDY_USER_MODELS_DIR}"
				"$<TARGET_FILE:pam_installed_e2e_driver>"
				"$<TARGET_FILE:pam_installed_runtime_e2e_driver>"
		)
	else()
		add_test(
			NAME installed-pam-setuid-e2e
			COMMAND bash -c "echo 'SKIP: pam_start_confdir unavailable'; exit 77"
		)
		add_test(
			NAME installed-pam-setuid-install-environment
			COMMAND bash -c "echo 'SKIP: pam_start_confdir unavailable'; exit 77"
		)
	endif()
	set_tests_properties(
		installed-pam-setuid-e2e installed-pam-setuid-install-environment
		PROPERTIES
			RUN_SERIAL TRUE
			LABELS "privileged;e2e;pam;setuid"
			SKIP_RETURN_CODE 77
			TIMEOUT 180
	)
endif()
