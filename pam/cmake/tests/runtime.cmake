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
 pam_child_spawn_test
 pam-child-spawn
 tests/runtime/child_spawn_test.cpp
)
target_link_libraries(pam_child_spawn_test PRIVATE pam_runtime)
# Lets the test stand in for a kernel without clone3; harmless where glibc has no pidfd_spawn.
target_link_options(pam_child_spawn_test PRIVATE "LINKER:--wrap=pidfd_spawn")

howdy_add_pam_test(
 pam_runtime_session_test
 pam-runtime-session
 tests/runtime/runtime_session_test.cpp
 tests/runtime/runtime_session_spawn_test.cpp
 tests/runtime/runtime_session_deadline_test.cpp
)
target_link_libraries(
 pam_runtime_session_test
 PRIVATE pam_runtime pam_explicit_config_path
)
