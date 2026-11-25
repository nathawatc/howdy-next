howdy_add_native_test(
 howdy_clear_cli_test
 native-clear-cli
 tests/cli/clear_cli_test.cpp
)
target_link_libraries(howdy_clear_cli_test PRIVATE howdy_cli)

howdy_add_native_test(
 howdy_config_cli_test
 native-config-cli
 tests/cli/config_cli_test.cpp
 tests/cli/config_cli_workflow_test.cpp
 tests/cli/config_cli_integration_test.cpp
)
target_link_options(howdy_config_cli_test PRIVATE "LINKER:--wrap=initgroups")
target_link_libraries(howdy_config_cli_test PRIVATE howdy_cli)

howdy_add_native_test(
 howdy_disable_cli_test
 native-disable-cli
 tests/cli/disable_cli_test.cpp
)
target_link_libraries(howdy_disable_cli_test PRIVATE howdy_cli)

howdy_add_native_test(
 howdy_set_cli_test
 native-set-cli
 tests/cli/set_cli_test.cpp
)
target_link_libraries(howdy_set_cli_test PRIVATE howdy_cli)

howdy_add_native_test(
 howdy_list_cli_test
 native-list-cli
 tests/cli/list_cli_test.cpp
)
target_link_libraries(howdy_list_cli_test PRIVATE howdy_cli)

howdy_add_native_test(
 howdy_add_cli_test
 native-add-cli
 tests/cli/add_cli_test.cpp
 tests/cli/add_cli_preflight_test.cpp
 tests/cli/add_cli_capture_test.cpp
 tests/cli/add_cli_arguments_test.cpp
)
target_link_libraries(howdy_add_cli_test PRIVATE howdy_cli howdy_opencv)

howdy_add_native_test(
 howdy_enrollment_capture_test
 native-enrollment-capture
 tests/cli/enrollment_capture_test.cpp
)
target_link_libraries(
 howdy_enrollment_capture_test
 PRIVATE howdy_frame_processing howdy_opencv
)

howdy_add_native_test(
 howdy_remove_cli_test
 native-remove-cli
 tests/cli/remove_cli_test.cpp
)
target_link_libraries(howdy_remove_cli_test PRIVATE howdy_cli)

howdy_add_native_test(
 howdy_test_cli_test
 native-test-cli
 tests/cli/test_cli_test.cpp
)
target_link_libraries(howdy_test_cli_test PRIVATE howdy_cli howdy_opencv)

howdy_add_native_test(
 howdy_test_gui_probe_test
 native-test-gui-probe
 tests/cli/test_gui_probe_test.cpp
)
target_link_libraries(howdy_test_gui_probe_test PRIVATE howdy_cli)

howdy_add_native_test(
 howdy_privilege_drop_test
 native-privilege-drop
 tests/cli/privilege_drop_test.cpp
)
target_link_libraries(howdy_privilege_drop_test PRIVATE howdy_cli)

howdy_add_native_test(
 howdy_test_preview_session_test
 native-test-preview-session
 tests/cli/test_preview_session_test.cpp
 tests/cli/test_preview_renderer_test.cpp
)
target_link_libraries(
 howdy_test_preview_session_test
 PRIVATE
  howdy_test_preview
  howdy_preview_engine
  howdy_opencv
)

howdy_add_native_test(
 howdy_snapshot_cli_test
 native-snapshot-cli
 tests/cli/snapshot_cli_test.cpp
)
target_link_libraries(howdy_snapshot_cli_test PRIVATE howdy_cli howdy_opencv)

howdy_add_native_test(
 howdy_snapshot_writer_test
 native-snapshot-writer
 tests/cli/snapshot_writer_test.cpp
 tests/cli/snapshot_writer_security_test.cpp
)
target_link_libraries(
 howdy_snapshot_writer_test PRIVATE howdy_cli howdy_opencv
)

howdy_add_native_test(
 howdy_download_models_test
 native-download-models
 tests/cli/download_models_test.cpp
 tests/cli/download_models_entrypoint_test.cpp
 tests/cli/download_models_integrity_test.cpp
 tests/cli/download_models_manifest_test.cpp
 tests/cli/download_models_proxy_test.cpp
 tests/cli/download_models_test_support.cpp
)
target_link_libraries(howdy_download_models_test PRIVATE howdy_cli
                       PkgConfig::LIBCURL Threads::Threads)
