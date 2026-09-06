install(SCRIPT "${PROJECT_BINARY_DIR}/cmake_install_prefix_guard.cmake")
add_subdirectory(po)

add_library(pam_options STATIC src/module/pam_options.cpp)
howdy_configure_pam_target(pam_options)
target_link_libraries(pam_options PRIVATE howdy_pam_option_catalog)

add_library(pam_message_locale STATIC src/runtime/message_locale.cpp)
howdy_configure_pam_target(pam_message_locale)
target_include_directories(
	pam_message_locale
	PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

add_library(
	pam_runtime STATIC
	src/runtime/auth_helper_process.cpp
	src/runtime/compare_process.cpp
	src/runtime/environment.cpp
	src/runtime/lid_probe.cpp
	src/runtime/runtime_session.cpp
	src/runtime/session_probe.cpp
)
howdy_configure_pam_target(pam_runtime)
target_compile_definitions(
	pam_runtime PRIVATE HOWDY_RUNTIME_CONFIG_EXPLICIT_PATH_ONLY
)
target_link_libraries(pam_runtime PUBLIC PAM::PAM PRIVATE howdy_config)

add_library(
	pam_prompt STATIC
	src/prompt/prompt_submitter.cpp
	src/prompt/conversation_response.cpp
	src/prompt/pam_conversation.cpp
	src/prompt/internal_fd.cpp
	src/prompt/native_prompt_conversation.cpp
	src/prompt/native_prompt_input.cpp
	src/prompt/observed_prompt_conversation.cpp
	src/prompt/prompt_coordinator.cpp
	src/prompt/workaround.cpp
)
howdy_configure_pam_target(pam_prompt)
target_link_libraries(
	pam_prompt
	PUBLIC PAM::PAM Threads::Threads
	PRIVATE PkgConfig::LIBEVDEV pam_runtime
)

add_library(
	pam_module STATIC
	src/module/auth_eligibility.cpp
	src/module/auth_flow.cpp
	src/module/status_mapping.cpp
)
howdy_configure_pam_target(pam_module)
target_compile_definitions(
	pam_module PRIVATE HOWDY_RUNTIME_CONFIG_EXPLICIT_PATH_ONLY
)
target_link_libraries(
	pam_module
	PUBLIC PAM::PAM
	PRIVATE pam_options pam_prompt pam_runtime howdy_user_model_readiness
)

add_library(pam_entrypoint STATIC src/module/entrypoint.cpp)
howdy_configure_pam_target(pam_entrypoint)
target_link_libraries(
	pam_entrypoint
	PUBLIC PAM::PAM
	PRIVATE pam_message_locale
)

add_library(
	pam_production_entrypoint STATIC
	src/module/production_entrypoint.cpp
)
howdy_configure_pam_target(pam_production_entrypoint)
target_link_libraries(
	pam_production_entrypoint
	PUBLIC pam_entrypoint
	PRIVATE pam_module
)

add_library(
	pam_howdy MODULE
	src/module/main.cpp
	src/module/unsupported_entrypoints.cpp
)
howdy_configure_pam_target(pam_howdy)
target_compile_definitions(
	pam_howdy PRIVATE HOWDY_RUNTIME_CONFIG_EXPLICIT_PATH_ONLY
)
target_link_libraries(pam_howdy PRIVATE pam_production_entrypoint)
set_target_properties(pam_howdy PROPERTIES PREFIX "" OUTPUT_NAME "pam_howdy")
install(
	TARGETS pam_howdy
	LIBRARY DESTINATION "${HOWDY_PAM_DIR}"
)

if(BUILD_TESTING)
	set(pam_integration_dir "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration")
	function(howdy_add_pam_test target test_name)
		add_executable(${target} ${ARGN})
		howdy_configure_pam_test_target(${target})
		add_test(NAME ${test_name} COMMAND ${target})
	endfunction()

	howdy_add_pam_test(
		pam_options_test
		pam-options
		tests/module/pam_options_test.cpp
	)
	target_link_libraries(pam_options_test PRIVATE pam_options)

	howdy_add_pam_test(
		pam_message_locale_test
		pam-message-locale
		tests/runtime/message_locale_test.cpp
	)
	target_link_libraries(pam_message_locale_test PRIVATE pam_message_locale)

	howdy_add_pam_test(
		pam_entrypoint_test
		pam-entrypoint
		tests/module/entrypoint_test.cpp
	)
	target_link_libraries(pam_entrypoint_test PRIVATE pam_entrypoint)

	howdy_add_pam_test(
		pam_status_mapping_test
		pam-status-mapping
		tests/module/status_mapping_test.cpp
	)
	target_link_libraries(pam_status_mapping_test PRIVATE pam_module)
	target_compile_definitions(
		pam_status_mapping_test
		PRIVATE HOWDY_TEST_LOCALEDIR="${CMAKE_CURRENT_BINARY_DIR}/po"
	)
	add_dependencies(pam_status_mapping_test howdy-gmo)

	howdy_add_pam_test(
		pam_environment_test
		pam-environment
		tests/runtime/environment_test.cpp
	)
	target_link_libraries(pam_environment_test PRIVATE pam_runtime)

	howdy_add_pam_test(
		pam_session_probe_test
		pam-session-probe
		tests/runtime/session_probe_test.cpp
	)
	target_link_libraries(pam_session_probe_test PRIVATE pam_runtime)

	howdy_add_pam_test(
		pam_lid_probe_test
		pam-lid-probe
		tests/runtime/lid_probe_test.cpp
	)
	target_link_libraries(pam_lid_probe_test PRIVATE pam_runtime)

	howdy_add_pam_test(
		pam_auth_eligibility_test
		pam-auth-eligibility
		tests/module/auth_eligibility_test.cpp
	)
	target_link_libraries(pam_auth_eligibility_test PRIVATE pam_module)

	howdy_add_pam_test(
		pam_prompt_workaround_test
		pam-prompt-workaround
		tests/prompt/prompt_workaround_test.cpp
	)
	target_link_libraries(pam_prompt_workaround_test PRIVATE pam_prompt)

	howdy_add_pam_test(
		pam_abi_entrypoints_test
		pam-abi-entrypoints
		tests/module/abi_entrypoints_test.cpp
		src/module/unsupported_entrypoints.cpp
	)
	target_link_libraries(pam_abi_entrypoints_test PRIVATE pam_entrypoint)
	howdy_add_pam_test(
		pam_internal_fd_test
		pam-internal-fd
		tests/prompt/internal_fd_test.cpp
	)
	target_link_libraries(pam_internal_fd_test PRIVATE pam_prompt)
	target_include_directories(
		pam_internal_fd_test
		PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/prompt"
	)
	howdy_add_pam_test(
		pam_native_prompt_conversation_test
		pam-native-prompt-conversation
		tests/prompt/native_prompt_conversation_test.cpp
		tests/prompt/native_prompt_terminal_test.cpp
		tests/prompt/native_prompt_fd_test.cpp
		tests/prompt/native_prompt_input_test.cpp
	)
	target_link_libraries(pam_native_prompt_conversation_test PRIVATE pam_prompt)
	target_include_directories(
		pam_native_prompt_conversation_test
		PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
	)
	howdy_add_pam_test(
		pam_observed_prompt_conversation_test
		pam-observed-prompt-conversation
		tests/prompt/observed_prompt_conversation_test.cpp
	)
	target_link_libraries(pam_observed_prompt_conversation_test PRIVATE pam_prompt)
	howdy_add_pam_test(
		pam_conversation_response_test
		pam-conversation-response
		tests/prompt/conversation_response_test.cpp
	)
	target_link_libraries(pam_conversation_response_test PRIVATE pam_prompt)
	target_include_directories(
		pam_conversation_response_test
		PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/prompt"
	)
	howdy_add_pam_test(
		pam_conversation_test
		pam-conversation
		tests/prompt/pam_conversation_test.cpp
	)
	target_link_libraries(pam_conversation_test PRIVATE pam_prompt)
	howdy_add_pam_test(
		pam_auth_flow_helpers_test
		pam-auth-flow-helpers
		tests/module/auth_flow_helpers_test.cpp
		tests/module/auth_flow_integration_test.cpp
		tests/runtime/auth_helper_output_test.cpp
		tests/runtime/process_wait_test.cpp
	)
	target_compile_definitions(
		pam_auth_flow_helpers_test
		PRIVATE
			HOWDY_RUNTIME_CONFIG_EXPLICIT_PATH_ONLY
	)
	target_link_libraries(
		pam_auth_flow_helpers_test
		PRIVATE pam_entrypoint pam_module
	)

	howdy_add_pam_test(
		pam_prompt_coordinator_test
		pam-prompt-coordinator
		tests/prompt/prompt_coordinator_test.cpp
		tests/prompt/prompt_coordinator_modes_test.cpp
		tests/prompt/prompt_coordinator_adapter_test.cpp
	)
	target_link_libraries(
		pam_prompt_coordinator_test
		PRIVATE pam_prompt pam_runtime
	)

	howdy_add_pam_test(
		pam_runtime_session_test
		pam-runtime-session
		tests/runtime/runtime_session_test.cpp
		tests/runtime/runtime_session_spawn_test.cpp
		tests/runtime/runtime_session_deadline_test.cpp
	)
	target_compile_definitions(
		pam_runtime_session_test
		PRIVATE
			HOWDY_RUNTIME_CONFIG_EXPLICIT_PATH_ONLY
	)
	target_link_libraries(pam_runtime_session_test PRIVATE pam_runtime)
	add_test(
		NAME pam-installed-setuid-path-validation
		COMMAND
			bash
			"${pam_integration_dir}/installed_pam_setuid_e2e.sh"
			--validate-paths
	)
	howdy_set_test_timeouts()

	if(HOWDY_ENABLE_PRIVILEGED_TESTS)
		include(CheckCXXSourceCompiles)
		cmake_push_check_state(RESET)
		set(CMAKE_REQUIRED_LIBRARIES PAM::PAM)
		check_cxx_source_compiles(
			[=[
				#include <security/pam_appl.h>
				int main() {
					pam_handle_t *handle = nullptr;
					pam_conv conversation{};
					return pam_start_confdir(
						"howdy-e2e", "nobody", &conversation, "/tmp", &handle);
				}
			]=]
			HOWDY_HAVE_PAM_START_CONFDIR
		)
		cmake_pop_check_state()

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
endif()
