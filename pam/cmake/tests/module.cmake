howdy_add_pam_test(
	pam_options_test
	pam-options
	tests/module/pam_options_test.cpp
)
target_link_libraries(pam_options_test PRIVATE pam_options)

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
	pam_auth_eligibility_test
	pam-auth-eligibility
	tests/module/auth_eligibility_test.cpp
)
target_link_libraries(pam_auth_eligibility_test PRIVATE pam_module)

howdy_add_pam_test(
	pam_abi_entrypoints_test
	pam-abi-entrypoints
	tests/module/abi_entrypoints_test.cpp
	src/module/unsupported_entrypoints.cpp
)
target_link_libraries(pam_abi_entrypoints_test PRIVATE pam_entrypoint)

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
