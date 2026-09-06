add_executable(howdy_clear_cli_test tests/cli/clear_cli_test.cpp)
howdy_configure_native_test_target(howdy_clear_cli_test)
target_link_libraries(howdy_clear_cli_test PRIVATE howdy_cli)
add_test(NAME native-clear-cli COMMAND howdy_clear_cli_test)

add_executable(
	howdy_config_cli_test
	tests/cli/config_cli_test.cpp
	tests/cli/config_cli_workflow_test.cpp
	tests/cli/config_cli_integration_test.cpp
)
howdy_configure_native_test_target(howdy_config_cli_test)
target_link_options(howdy_config_cli_test PRIVATE -Wl,--wrap=initgroups)
target_link_libraries(howdy_config_cli_test PRIVATE howdy_cli)
add_test(NAME native-config-cli COMMAND howdy_config_cli_test)

add_executable(howdy_disable_cli_test tests/cli/disable_cli_test.cpp)
howdy_configure_native_test_target(howdy_disable_cli_test)
target_link_libraries(howdy_disable_cli_test PRIVATE howdy_cli)
add_test(NAME native-disable-cli COMMAND howdy_disable_cli_test)

add_executable(howdy_set_cli_test tests/cli/set_cli_test.cpp)
howdy_configure_native_test_target(howdy_set_cli_test)
target_link_libraries(howdy_set_cli_test PRIVATE howdy_cli)
add_test(NAME native-set-cli COMMAND howdy_set_cli_test)

add_executable(howdy_list_cli_test tests/cli/list_cli_test.cpp)
howdy_configure_native_test_target(howdy_list_cli_test)
target_link_libraries(howdy_list_cli_test PRIVATE howdy_cli)
add_test(NAME native-list-cli COMMAND howdy_list_cli_test)

add_executable(
	howdy_add_cli_test
	tests/cli/add_cli_test.cpp
	tests/cli/add_cli_preflight_test.cpp
	tests/cli/add_cli_capture_test.cpp
	tests/cli/add_cli_arguments_test.cpp
)
howdy_configure_native_test_target(howdy_add_cli_test)
target_link_libraries(howdy_add_cli_test PRIVATE howdy_cli)
add_test(NAME native-add-cli COMMAND howdy_add_cli_test)

add_executable(howdy_remove_cli_test tests/cli/remove_cli_test.cpp)
howdy_configure_native_test_target(howdy_remove_cli_test)
target_link_libraries(howdy_remove_cli_test PRIVATE howdy_cli)
add_test(NAME native-remove-cli COMMAND howdy_remove_cli_test)

add_executable(howdy_test_cli_test tests/cli/test_cli_test.cpp)
howdy_configure_native_test_target(howdy_test_cli_test)
target_link_libraries(howdy_test_cli_test PRIVATE howdy_cli)
add_test(NAME native-test-cli COMMAND howdy_test_cli_test)

add_executable(
	howdy_test_preview_session_test
	tests/cli/test_preview_session_test.cpp
	tests/cli/test_preview_renderer_test.cpp
)
howdy_configure_native_test_target(howdy_test_preview_session_test)
target_link_libraries(
	howdy_test_preview_session_test
	PRIVATE
		howdy_cli
		howdy_test_preview
		howdy_preview_engine
		howdy_opencv
)
add_test(
	NAME native-test-preview-session COMMAND howdy_test_preview_session_test
)

add_executable(howdy_snapshot_cli_test tests/cli/snapshot_cli_test.cpp)
howdy_configure_native_test_target(howdy_snapshot_cli_test)
target_link_libraries(howdy_snapshot_cli_test PRIVATE howdy_cli howdy_opencv)
add_test(NAME native-snapshot-cli COMMAND howdy_snapshot_cli_test)

add_executable(howdy_snapshot_writer_test tests/cli/snapshot_writer_test.cpp)
howdy_configure_native_test_target(howdy_snapshot_writer_test)
target_link_libraries(
	howdy_snapshot_writer_test PRIVATE howdy_cli howdy_opencv
)
add_test(NAME native-snapshot-writer COMMAND howdy_snapshot_writer_test)

add_executable(
	howdy_download_models_test
	tests/cli/download_models_test.cpp
	tests/cli/download_models_entrypoint_test.cpp
	tests/cli/download_models_integrity_test.cpp
	tests/cli/download_models_manifest_test.cpp
	tests/cli/download_models_proxy_test.cpp
	tests/cli/download_models_test_support.cpp
	tests/support/atomic_files_test.cpp
)
howdy_configure_native_test_target(howdy_download_models_test)
target_link_libraries(howdy_download_models_test PRIVATE howdy_cli
                       Threads::Threads)
add_test(NAME native-download-models COMMAND howdy_download_models_test)
