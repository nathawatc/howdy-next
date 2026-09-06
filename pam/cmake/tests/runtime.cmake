howdy_add_pam_test(
	pam_message_locale_test
	pam-message-locale
	tests/runtime/message_locale_test.cpp
)
target_link_libraries(pam_message_locale_test PRIVATE pam_message_locale)

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
