install(SCRIPT "${PROJECT_BINARY_DIR}/cmake_install_prefix_guard.cmake")
add_subdirectory(po)

set(
	HOWDY_PAM_RUNTIME_CONFIG_SOURCES
	"${PROJECT_SOURCE_DIR}/howdy/src/config/config_reader.cpp"
	"${PROJECT_SOURCE_DIR}/howdy/src/config/runtime_config.cpp"
	"${PROJECT_SOURCE_DIR}/howdy/src/config/config_schema.cpp"
	"${PROJECT_SOURCE_DIR}/howdy/src/config/config_validation.cpp"
)
set(
	HOWDY_PAM_RUNTIME_SOURCES
	${HOWDY_PAM_RUNTIME_CONFIG_SOURCES}
	"${PROJECT_SOURCE_DIR}/howdy/src/storage/user_model_readiness.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/auth_flow.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/auth_helper_process.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/compare_process.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/runtime_session.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/enter_device.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/native_prompt_conversation.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/native_prompt_input.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/prompt_coordinator.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/prompt_workaround.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/status_mapping.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/tty_restore.cpp"
)
set(
	HOWDY_PAM_LIBRARIES
	PAM::PAM
	PkgConfig::INIREADER
	Threads::Threads
	PkgConfig::LIBEVDEV
)

add_library(pam_howdy MODULE ${HOWDY_PAM_RUNTIME_SOURCES})
howdy_configure_pam_target(pam_howdy)
target_compile_definitions(pam_howdy PRIVATE HOWDY_RUNTIME_CONFIG_EXPLICIT_PATH_ONLY)
target_link_libraries(pam_howdy PRIVATE ${HOWDY_PAM_LIBRARIES})
set_target_properties(pam_howdy PROPERTIES PREFIX "" OUTPUT_NAME "pam_howdy")
install(
	TARGETS pam_howdy
	LIBRARY DESTINATION "${HOWDY_PAM_DIR}"
)

if(BUILD_TESTING)
	function(howdy_add_pam_test target test_name)
		add_executable(${target} ${ARGN})
		howdy_configure_pam_test_target(${target})
		target_link_libraries(${target} PRIVATE ${HOWDY_PAM_LIBRARIES})
		add_test(NAME ${test_name} COMMAND ${target})
	endfunction()

	howdy_add_pam_test(
		pam_status_mapping_test
		pam-status-mapping
		src/status_mapping.cpp
		src/tests/status_mapping_test.cpp
	)
	target_compile_definitions(
		pam_status_mapping_test PRIVATE HOWDY_TEST_LOCALEDIR="${CMAKE_CURRENT_BINARY_DIR}/po"
	)
	add_dependencies(pam_status_mapping_test howdy-gmo)

	howdy_add_pam_test(
		pam_prompt_workaround_test
		pam-prompt-workaround
		src/prompt_workaround.cpp
		src/tests/prompt_workaround_test.cpp
	)

	howdy_add_pam_test(
		pam_main_entrypoints_test
		pam-main-entrypoints
		src/main.cpp
		src/tests/main_entrypoints_test.cpp
	)
	target_compile_definitions(
		pam_main_entrypoints_test PRIVATE HOWDY_TEST_LOCALEDIR="${CMAKE_CURRENT_BINARY_DIR}/po"
	)
	add_dependencies(pam_main_entrypoints_test howdy-gmo)

	howdy_add_pam_test(
		pam_native_prompt_conversation_test
		pam-native-prompt-conversation
		src/native_prompt_conversation.cpp
		src/native_prompt_input.cpp
		src/tests/native_prompt_conversation_test.cpp
	)
	target_compile_definitions(pam_native_prompt_conversation_test PRIVATE HOWDY_PAM_TESTING)

	add_executable(pam_optional_task_test src/tests/optional_task_test.cpp)
	howdy_configure_pam_test_target(pam_optional_task_test)
	target_link_libraries(pam_optional_task_test PRIVATE Threads::Threads)
	add_test(NAME pam-optional-task COMMAND pam_optional_task_test)
	add_test(
		NAME pam-optional-task-get-before-spawn
		COMMAND pam_optional_task_test get-before-spawn
	)
	add_test(
		NAME pam-optional-task-get-while-active
		COMMAND pam_optional_task_test get-while-active
	)

	add_executable(
		pam_tty_restore_test
		src/tty_restore.cpp
		src/tests/tty_restore_test.cpp
	)
	howdy_configure_pam_test_target(pam_tty_restore_test)
	target_link_libraries(pam_tty_restore_test PRIVATE PAM::PAM)
	add_test(NAME pam-tty-restore COMMAND pam_tty_restore_test)

	howdy_add_pam_test(
		pam_auth_flow_helpers_test
		pam-auth-flow-helpers
		${HOWDY_PAM_RUNTIME_CONFIG_SOURCES}
		"${PROJECT_SOURCE_DIR}/howdy/src/storage/user_model_readiness.cpp"
		src/auth_flow.cpp
		src/auth_helper_process.cpp
		src/compare_process.cpp
		src/main.cpp
		src/runtime_session.cpp
		src/enter_device.cpp
		src/native_prompt_conversation.cpp
		src/native_prompt_input.cpp
		src/prompt_coordinator.cpp
		src/prompt_workaround.cpp
		src/status_mapping.cpp
		src/tests/auth_flow_helpers_test.cpp
	)
	target_compile_definitions(
		pam_auth_flow_helpers_test
		PRIVATE
			HOWDY_PAM_TESTING
			HOWDY_RUNTIME_CONFIG_EXPLICIT_PATH_ONLY
	)

	add_executable(
		pam_prompt_coordinator_test
		src/compare_process.cpp
		src/prompt_coordinator.cpp
		src/enter_device.cpp
		src/native_prompt_conversation.cpp
		src/native_prompt_input.cpp
		src/prompt_workaround.cpp
		src/tests/prompt_coordinator_test.cpp
	)
	howdy_configure_pam_test_target(pam_prompt_coordinator_test)
	target_compile_definitions(pam_prompt_coordinator_test PRIVATE HOWDY_PAM_TESTING)
	target_link_libraries(
		pam_prompt_coordinator_test
		PRIVATE
			PAM::PAM
			Threads::Threads
			PkgConfig::LIBEVDEV
	)
	add_test(NAME pam-prompt-coordinator COMMAND pam_prompt_coordinator_test)

	howdy_add_pam_test(
		pam_runtime_session_test
		pam-runtime-session
		${HOWDY_PAM_RUNTIME_CONFIG_SOURCES}
		src/auth_helper_process.cpp
		src/runtime_session.cpp
		src/tests/runtime_session_test.cpp
	)
	target_compile_definitions(
		pam_runtime_session_test
		PRIVATE
			HOWDY_PAM_TESTING
			HOWDY_RUNTIME_CONFIG_EXPLICIT_PATH_ONLY
	)
	add_test(
		NAME pam-installed-setuid-path-validation
		COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/src/tests/installed_pam_setuid_e2e.sh" --validate-paths
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
					return pam_start_confdir("howdy-e2e", "nobody", &conversation, "/tmp", &handle);
				}
			]=]
			HOWDY_HAVE_PAM_START_CONFDIR
		)
		cmake_pop_check_state()

		if(HOWDY_HAVE_PAM_START_CONFDIR)
			add_executable(pam_installed_e2e_driver src/tests/installed_pam_driver.cpp)
			howdy_configure_pam_target(pam_installed_e2e_driver)
			target_link_libraries(pam_installed_e2e_driver PRIVATE PAM::PAM)
			add_test(
				NAME installed-pam-setuid-e2e
				COMMAND
					bash "${CMAKE_CURRENT_SOURCE_DIR}/src/tests/installed_pam_setuid_e2e.sh"
					"${PROJECT_BINARY_DIR}"
					"${CMAKE_INSTALL_PREFIX}"
					"${HOWDY_PAM_DIR}"
					"${HOWDY_HELPER_DIR}"
					"${HOWDY_CONFIG_DIR}"
					"${HOWDY_USER_MODELS_DIR}"
					"$<TARGET_FILE:pam_installed_e2e_driver>"
			)
			add_test(
				NAME installed-pam-setuid-install-environment
				COMMAND
					sh
					"${CMAKE_CURRENT_SOURCE_DIR}/src/tests/installed_pam_install_environment_test.sh"
					"${CMAKE_CURRENT_SOURCE_DIR}/src/tests/installed_pam_setuid_e2e.sh"
					"${PROJECT_BINARY_DIR}"
					"${CMAKE_INSTALL_PREFIX}"
					"${HOWDY_PAM_DIR}"
					"${HOWDY_HELPER_DIR}"
					"${HOWDY_CONFIG_DIR}"
					"${HOWDY_USER_MODELS_DIR}"
					"$<TARGET_FILE:pam_installed_e2e_driver>"
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
