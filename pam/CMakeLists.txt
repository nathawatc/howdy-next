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
	src/runtime/auth_helper_process/internal.hpp
	src/runtime/auth_helper_process/io.cpp
	src/runtime/auth_helper_process/lease.cpp
	src/runtime/auth_helper_process/spawn.cpp
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
target_link_libraries(pam_runtime PUBLIC PAM::PAM PRIVATE howdy_runtime_config_loader)

add_library(
	pam_conversation_response STATIC
	src/prompt/conversation_response.cpp
)
howdy_configure_pam_target(pam_conversation_response)
target_link_libraries(pam_conversation_response PUBLIC PAM::PAM)

add_library(
	pam_prompt STATIC
	src/prompt/prompt_submitter.cpp
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
	PRIVATE PkgConfig::LIBEVDEV pam_conversation_response pam_runtime
)

add_library(
	pam_module STATIC
	src/module/auth_eligibility.cpp
	src/module/auth_flow.cpp
	src/module/status_mapping.cpp
)
howdy_configure_pam_target(pam_module)
target_compile_definitions(
	pam_module
	PRIVATE
		HOWDY_RUNTIME_CONFIG_EXPLICIT_PATH_ONLY
		LOCALEDIR="${CMAKE_INSTALL_FULL_LOCALEDIR}"
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
	src/module/main/unsupported_entrypoints.cpp
)
howdy_configure_pam_target(pam_howdy)
target_compile_definitions(
	pam_howdy PRIVATE HOWDY_RUNTIME_CONFIG_EXPLICIT_PATH_ONLY
)
target_link_libraries(pam_howdy PRIVATE pam_production_entrypoint)
target_link_options(pam_howdy PRIVATE "LINKER:--exclude-libs,ALL")
set_target_properties(pam_howdy PROPERTIES PREFIX "" OUTPUT_NAME "pam_howdy")
install(
	TARGETS pam_howdy
	LIBRARY DESTINATION "${HOWDY_PAM_DIR}"
)

if(BUILD_TESTING)
	function(howdy_add_pam_test target test_name)
		add_executable(${target} ${ARGN})
		howdy_configure_pam_test_target(${target})
		add_test(NAME ${test_name} COMMAND ${target})
	endfunction()

	include(cmake/tests/module.cmake)
	include(cmake/tests/runtime.cmake)
	include(cmake/tests/prompt.cmake)
	include(cmake/tests/integration.cmake)
endif()
