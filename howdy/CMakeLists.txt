install(SCRIPT "${PROJECT_BINARY_DIR}/cmake_install_prefix_guard.cmake")
add_library(
	howdy_config
	STATIC
	src/config/config_reader.cpp
	src/config/runtime_config.cpp
	src/config/config_schema.cpp
	src/config/config_utils.cpp
	src/config/config_validation.cpp
)
howdy_configure_native_target(howdy_config)
target_link_libraries(howdy_config PUBLIC PkgConfig::INIREADER)

add_library(
	howdy_runtime_paths
	STATIC
	src/config/runtime_paths.cpp
	src/config/runtime_config_defaults.cpp
)
howdy_configure_native_target(howdy_runtime_paths)
target_link_libraries(howdy_runtime_paths PUBLIC howdy_config)

add_library(howdy_user_model_readiness STATIC src/storage/user_model_readiness.cpp)
howdy_configure_native_target(howdy_user_model_readiness)

add_library(howdy_model_file STATIC src/model_assets/model_file.cpp)
howdy_configure_native_target(howdy_model_file)

add_library(howdy_compare_args STATIC src/compare/args.cpp)
howdy_configure_native_target(howdy_compare_args)

add_library(howdy_compare_logic STATIC src/compare/logic.cpp)
howdy_configure_native_target(howdy_compare_logic)
target_link_libraries(howdy_compare_logic PUBLIC howdy_opencv)

add_library(
	howdy_vision_core
	STATIC
	src/vision/face_detection.cpp
	src/vision/face_encoding.cpp
	src/vision/video_capture.cpp
)
howdy_configure_native_target(howdy_vision_core)
target_link_libraries(howdy_vision_core PUBLIC howdy_opencv)

add_library(howdy_frame_processing STATIC src/vision/frame_processing.cpp)
howdy_configure_native_target(howdy_frame_processing)
target_link_libraries(howdy_frame_processing PUBLIC howdy_opencv)

add_library(howdy_preview_engine STATIC src/vision/preview_engine.cpp)
howdy_configure_native_target(howdy_preview_engine)
target_link_libraries(howdy_preview_engine PUBLIC howdy_opencv)

add_library(
	howdy_test_preview
	STATIC
	src/cli/test_preview_session.cpp
	src/cli/test_preview_renderer.cpp
)
howdy_configure_native_target(howdy_test_preview)
target_link_libraries(
	howdy_test_preview
	PUBLIC
		howdy_preview_engine
		howdy_frame_processing
		howdy_opencv
)

add_library(howdy_compare_engine STATIC src/compare/engine.cpp)
howdy_configure_native_target(howdy_compare_engine)
target_link_libraries(howdy_compare_engine PUBLIC howdy_opencv)

add_library(howdy_compare_capture_session STATIC src/compare/capture_session.cpp)
howdy_configure_native_target(howdy_compare_capture_session)
target_link_libraries(howdy_compare_capture_session PUBLIC howdy_opencv)

add_library(howdy_compare_privileges STATIC src/compare/privileges.cpp)
howdy_configure_native_target(howdy_compare_privileges)

add_library(howdy_compare_sandbox STATIC src/compare/sandbox.cpp)
howdy_configure_native_target(howdy_compare_sandbox)

add_library(howdy_compare_processing STATIC src/compare/processing.cpp)
howdy_configure_native_target(howdy_compare_processing)
target_link_libraries(howdy_compare_processing PUBLIC howdy_opencv)

add_library(howdy_user_model_codec STATIC src/storage/user_model_codec.cpp)
howdy_configure_native_target(howdy_user_model_codec)
target_link_libraries(howdy_user_model_codec PRIVATE PkgConfig::YYJSON)

add_library(
	howdy_native_cpp STATIC
	src/vision/face_model.cpp
	src/storage/user_model_store.cpp
	src/storage/user_models.cpp
)
howdy_configure_native_target(howdy_native_cpp)
target_link_libraries(
	howdy_native_cpp
	PUBLIC
		howdy_config
		howdy_runtime_paths
		howdy_user_model_readiness
		howdy_model_file
		howdy_vision_core
		howdy_user_model_codec
)

add_executable(howdy_compare src/bin/compare.cpp)
howdy_configure_native_target(howdy_compare)
target_link_libraries(
	howdy_compare
	PRIVATE
		howdy_compare_args
		howdy_compare_processing
		howdy_compare_privileges
		howdy_compare_sandbox
		howdy_compare_capture_session
		howdy_native_cpp
		howdy_compare_engine
		howdy_frame_processing
		howdy_compare_logic
)
set_target_properties(howdy_compare PROPERTIES OUTPUT_NAME "howdy-compare")
install(
	TARGETS howdy_compare
	RUNTIME DESTINATION "${HOWDY_HELPER_DIR}"
)

add_library(
	howdy_auth_helper_core STATIC
	src/auth_helper/acl.cpp
	src/auth_helper/runtime.cpp
	src/auth_helper/command.cpp
)
howdy_configure_native_target(howdy_auth_helper_core)
target_link_libraries(
	howdy_auth_helper_core
	PRIVATE PkgConfig::LIBACL howdy_runtime_paths howdy_user_model_readiness
)

add_executable(howdy_auth_helper src/bin/auth_helper.cpp)
howdy_configure_native_target(howdy_auth_helper)
target_link_libraries(howdy_auth_helper PRIVATE howdy_auth_helper_core)
set_target_properties(howdy_auth_helper PROPERTIES OUTPUT_NAME "howdy-auth-helper")
install(
	TARGETS howdy_auth_helper
	RUNTIME DESTINATION "${HOWDY_HELPER_DIR}"
	PERMISSIONS
		OWNER_READ OWNER_WRITE OWNER_EXECUTE SETUID
		GROUP_READ GROUP_EXECUTE
		WORLD_READ WORLD_EXECUTE
)

add_library(
	howdy_cli
	STATIC
	src/app/howdy.cpp
	src/cli/add.cpp
	src/cli/clear.cpp
	src/cli/config.cpp
	src/cli/config_edit_session.cpp
	src/cli/disable.cpp
	src/cli/download_models.cpp
	src/cli/list.cpp
	src/cli/remove.cpp
	src/cli/set.cpp
	src/cli/snapshot.cpp
	src/cli/test.cpp
)
howdy_configure_native_target(howdy_cli)
target_link_libraries(
	howdy_cli
	PUBLIC
		howdy_native_cpp
		howdy_test_preview
		howdy_preview_engine
		howdy_frame_processing
		howdy_compare_logic
		PkgConfig::LIBCURL
		PkgConfig::OPENSSL
		PkgConfig::YYJSON
		howdy_opencv
)

if(BUILD_TESTING)
	add_test(NAME native-compare-help COMMAND howdy_compare --help)
	add_test(NAME native-auth-helper-help COMMAND howdy_auth_helper --help)
	add_test(NAME native-auth-helper-empty COMMAND howdy_auth_helper)
	howdy_expect_test_failure(native-auth-helper-empty)
	add_test(NAME native-auth-helper-unknown-command COMMAND howdy_auth_helper unknown alice)
	howdy_expect_test_failure(native-auth-helper-unknown-command)
	add_test(NAME native-auth-helper-prepare-guard COMMAND howdy_auth_helper prepare ../alice)
	howdy_expect_test_failure(native-auth-helper-prepare-guard)
	add_test(
		NAME native-auth-helper-cleanup-guard
		COMMAND howdy_auth_helper cleanup /tmp/not-howdy-runtime
	)
	howdy_expect_test_failure(native-auth-helper-cleanup-guard)

	add_executable(
		howdy_auth_helper_test
		tests/auth_helper/auth_helper_test.cpp
	)
	howdy_configure_native_test_target(howdy_auth_helper_test)
	target_link_libraries(howdy_auth_helper_test PRIVATE howdy_auth_helper_core)
	add_test(NAME native-auth-helper COMMAND howdy_auth_helper_test)

	add_test(NAME native-compare-unknown-arg COMMAND howdy_compare --unknown)
	howdy_expect_test_failure(native-compare-unknown-arg)
	add_test(NAME native-compare-missing-user COMMAND howdy_compare)
	howdy_expect_test_failure(native-compare-missing-user)
	add_test(NAME native-compare-no-model COMMAND howdy_compare coverage-user)
	set_tests_properties(
		native-compare-no-model
		PROPERTIES
			WILL_FAIL TRUE
			ENVIRONMENT "HOWDY_USER_MODELS_DIR=${CMAKE_CURRENT_BINARY_DIR}/compare-test-models-empty"
	)

	add_executable(howdy_compare_args_test tests/compare/compare_args_test.cpp)
	howdy_configure_native_test_target(howdy_compare_args_test)
	target_link_libraries(howdy_compare_args_test PRIVATE howdy_compare_args)
	add_test(NAME native-compare-args COMMAND howdy_compare_args_test)

	add_executable(howdy_compare_logic_test tests/compare/compare_logic_test.cpp)
	howdy_configure_native_test_target(howdy_compare_logic_test)
	target_link_libraries(howdy_compare_logic_test PRIVATE howdy_compare_logic howdy_opencv)
	add_test(NAME native-compare-logic COMMAND howdy_compare_logic_test)

	add_executable(howdy_frame_processing_test tests/vision/frame_processing_test.cpp)
	howdy_configure_native_test_target(howdy_frame_processing_test)
	target_link_libraries(
		howdy_frame_processing_test
		PRIVATE howdy_frame_processing howdy_compare_logic howdy_opencv
	)
	add_test(NAME native-frame-processing COMMAND howdy_frame_processing_test)

	add_executable(howdy_compare_engine_test tests/compare/compare_engine_test.cpp)
	howdy_configure_native_test_target(howdy_compare_engine_test)
	target_link_libraries(
		howdy_compare_engine_test
		PRIVATE
			howdy_compare_engine
			howdy_frame_processing
			howdy_compare_logic
			howdy_opencv
	)
	add_test(NAME native-compare-engine COMMAND howdy_compare_engine_test)

	add_executable(
		howdy_compare_capture_session_test
		tests/compare/compare_capture_session_test.cpp
	)
	howdy_configure_native_test_target(howdy_compare_capture_session_test)
	target_link_libraries(
		howdy_compare_capture_session_test
		PRIVATE
			howdy_compare_capture_session
			howdy_native_cpp
	)
	add_test(NAME native-compare-capture-session COMMAND howdy_compare_capture_session_test)

	add_executable(howdy_compare_privileges_test tests/compare/compare_privileges_test.cpp)
	howdy_configure_native_test_target(howdy_compare_privileges_test)
	target_link_libraries(howdy_compare_privileges_test PRIVATE howdy_compare_privileges)
	add_test(NAME native-compare-privileges COMMAND howdy_compare_privileges_test)

	add_executable(howdy_compare_processing_test tests/compare/compare_processing_test.cpp)
	howdy_configure_native_test_target(howdy_compare_processing_test)
	target_link_libraries(howdy_compare_processing_test PRIVATE howdy_compare_processing)
	add_test(NAME native-compare-processing COMMAND howdy_compare_processing_test)

	add_executable(howdy_compare_sandbox_test tests/compare/compare_sandbox_test.cpp)
	howdy_configure_native_test_target(howdy_compare_sandbox_test)
	target_link_libraries(howdy_compare_sandbox_test PRIVATE howdy_compare_sandbox)
	add_test(NAME native-compare-sandbox COMMAND howdy_compare_sandbox_test)

	add_executable(howdy_face_matching_test tests/vision/face_matching_test.cpp)
	howdy_configure_native_test_target(howdy_face_matching_test)
	add_test(NAME native-face-matching COMMAND howdy_face_matching_test)

	add_executable(howdy_face_detection_test tests/vision/face_detection_test.cpp)
	howdy_configure_native_test_target(howdy_face_detection_test)
	target_link_libraries(howdy_face_detection_test PRIVATE howdy_vision_core)
	add_test(NAME native-face-detection COMMAND howdy_face_detection_test)

	add_executable(howdy_face_encoding_test tests/vision/face_encoding_test.cpp)
	howdy_configure_native_test_target(howdy_face_encoding_test)
	target_link_libraries(howdy_face_encoding_test PRIVATE howdy_vision_core)
	add_test(NAME native-face-encoding COMMAND howdy_face_encoding_test)

	add_executable(howdy_face_model_test tests/vision/face_model_test.cpp)
	howdy_configure_native_test_target(howdy_face_model_test)
	target_link_libraries(howdy_face_model_test PRIVATE howdy_native_cpp)
	add_test(NAME native-face-model COMMAND howdy_face_model_test)

	add_executable(howdy_config_utils_test tests/config/config_utils_test.cpp)
	howdy_configure_native_test_target(howdy_config_utils_test)
	target_link_libraries(howdy_config_utils_test PRIVATE howdy_native_cpp)
	add_test(NAME native-config-utils COMMAND howdy_config_utils_test)

	add_executable(howdy_file_security_test tests/support/file_security_test.cpp)
	howdy_configure_native_test_target(howdy_file_security_test)
	add_test(NAME native-file-security COMMAND howdy_file_security_test)

	add_executable(howdy_fd_io_test tests/support/fd_io_test.cpp)
	howdy_configure_native_test_target(howdy_fd_io_test)
	add_test(NAME native-fd-io COMMAND howdy_fd_io_test)

	add_executable(howdy_model_file_test tests/model_assets/model_file_test.cpp)
	howdy_configure_native_test_target(howdy_model_file_test)
	target_link_libraries(howdy_model_file_test PRIVATE howdy_model_file)
	add_test(NAME native-model-file COMMAND howdy_model_file_test)

	add_executable(howdy_capture_device_path_test tests/vision/capture_device_path_test.cpp)
	howdy_configure_native_test_target(howdy_capture_device_path_test)
	add_test(NAME native-capture-device-path COMMAND howdy_capture_device_path_test)

	add_executable(howdy_video_capture_test tests/vision/video_capture_test.cpp)
	howdy_configure_native_test_target(howdy_video_capture_test)
	target_link_libraries(howdy_video_capture_test PRIVATE howdy_vision_core)
	add_test(NAME native-video-capture COMMAND howdy_video_capture_test)

	add_executable(howdy_enrollment_capture_test tests/cli/enrollment_capture_test.cpp)
	howdy_configure_native_test_target(howdy_enrollment_capture_test)
	target_link_libraries(
		howdy_enrollment_capture_test
		PRIVATE howdy_frame_processing howdy_compare_logic howdy_opencv
	)
	add_test(NAME native-enrollment-capture COMMAND howdy_enrollment_capture_test)

	add_executable(howdy_config_validation_test tests/config/config_validation_test.cpp)
	howdy_configure_native_test_target(howdy_config_validation_test)
	target_compile_definitions(
		howdy_config_validation_test
		PRIVATE HOWDY_PACKAGED_CONFIG_PATH="${PROJECT_SOURCE_DIR}/config/config.ini"
	)
	target_link_libraries(
		howdy_config_validation_test
		PRIVATE howdy_native_cpp
	)
	add_test(NAME native-config-validation COMMAND howdy_config_validation_test)

	add_executable(howdy_user_models_test tests/storage/user_models_test.cpp)
	howdy_configure_native_test_target(howdy_user_models_test)
	target_link_libraries(howdy_user_models_test PRIVATE howdy_native_cpp)
	add_test(NAME native-user-models COMMAND howdy_user_models_test)

	add_executable(howdy_user_model_codec_test tests/storage/user_model_codec_test.cpp)
	howdy_configure_native_test_target(howdy_user_model_codec_test)
	target_link_libraries(howdy_user_model_codec_test PRIVATE howdy_user_model_codec)
	add_test(NAME native-user-model-codec COMMAND howdy_user_model_codec_test)

	add_executable(howdy_clear_cli_test tests/cli/clear_cli_test.cpp)
	howdy_configure_native_test_target(howdy_clear_cli_test)
	target_link_libraries(howdy_clear_cli_test PRIVATE howdy_cli)
	add_test(NAME native-clear-cli COMMAND howdy_clear_cli_test)

	add_executable(howdy_config_cli_test tests/cli/config_cli_test.cpp)
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

	add_executable(howdy_add_cli_test tests/cli/add_cli_test.cpp)
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

	add_executable(howdy_test_preview_session_test tests/cli/test_preview_session_test.cpp)
	howdy_configure_native_test_target(howdy_test_preview_session_test)
	target_link_libraries(
		howdy_test_preview_session_test
		PRIVATE
			howdy_cli
			howdy_test_preview
			howdy_preview_engine
			howdy_opencv
	)
	add_test(NAME native-test-preview-session COMMAND howdy_test_preview_session_test)

	add_executable(howdy_preview_engine_test tests/vision/preview_engine_test.cpp)
	howdy_configure_native_test_target(howdy_preview_engine_test)
	target_link_libraries(
		howdy_preview_engine_test
		PRIVATE
			howdy_native_cpp
			howdy_preview_engine
			howdy_frame_processing
			howdy_opencv
	)
	add_test(NAME native-preview-engine COMMAND howdy_preview_engine_test)

	add_executable(howdy_snapshot_cli_test tests/cli/snapshot_cli_test.cpp)
	howdy_configure_native_test_target(howdy_snapshot_cli_test)
	target_link_libraries(howdy_snapshot_cli_test PRIVATE howdy_cli howdy_opencv)
	add_test(NAME native-snapshot-cli COMMAND howdy_snapshot_cli_test)

	add_executable(howdy_snapshot_writer_test tests/cli/snapshot_writer_test.cpp)
	howdy_configure_native_test_target(howdy_snapshot_writer_test)
	target_link_libraries(howdy_snapshot_writer_test PRIVATE howdy_cli howdy_opencv)
	add_test(NAME native-snapshot-writer COMMAND howdy_snapshot_writer_test)

	add_executable(howdy_download_models_test tests/cli/download_models_test.cpp)
	howdy_configure_native_test_target(howdy_download_models_test)
	target_link_libraries(howdy_download_models_test PRIVATE howdy_cli)
	add_test(NAME native-download-models COMMAND howdy_download_models_test)

	add_executable(howdy_user_names_test tests/support/user_names_test.cpp)
	howdy_configure_native_test_target(howdy_user_names_test)
	add_test(NAME native-user-names COMMAND howdy_user_names_test)

	add_executable(howdy_runtime_paths_test tests/config/runtime_paths_test.cpp)
	howdy_configure_native_test_target(howdy_runtime_paths_test)
	target_link_libraries(
		howdy_runtime_paths_test
		PRIVATE howdy_native_cpp
	)
	add_test(NAME native-runtime-paths COMMAND howdy_runtime_paths_test)

	add_executable(howdy_runtime_config_load_test tests/config/runtime_config_load_test.cpp)
	howdy_configure_native_test_target(howdy_runtime_config_load_test)
	target_link_libraries(
		howdy_runtime_config_load_test
		PRIVATE howdy_native_cpp
	)
	add_test(NAME native-runtime-config-load COMMAND howdy_runtime_config_load_test)
	set_tests_properties(
		native-runtime-config-load
		PROPERTIES ENVIRONMENT "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}"
	)

	add_executable(howdy_runtime_config_test tests/config/runtime_config_test.cpp)
	howdy_configure_native_test_target(howdy_runtime_config_test)
	target_link_libraries(
		howdy_runtime_config_test
		PRIVATE howdy_native_cpp
	)
	add_test(NAME native-runtime-config COMMAND howdy_runtime_config_test)
	set_tests_properties(
		native-runtime-config
		PROPERTIES ENVIRONMENT "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}"
	)

	add_executable(howdy_invoking_user_env_test tests/support/invoking_user_env_test.cpp)
	howdy_configure_native_test_target(howdy_invoking_user_env_test)
	add_test(NAME native-invoking-user-env COMMAND howdy_invoking_user_env_test)

	add_executable(howdy_config_reader_test tests/config/config_reader_test.cpp)
	howdy_configure_native_test_target(howdy_config_reader_test)
	target_link_libraries(
		howdy_config_reader_test
		PRIVATE howdy_native_cpp
	)
	add_test(NAME native-config-reader COMMAND howdy_config_reader_test)
endif()

add_executable(howdy src/bin/howdy_main.cpp)
howdy_configure_native_target(howdy)
target_link_libraries(
	howdy
	PRIVATE
		howdy_cli
		PkgConfig::LIBCURL
		PkgConfig::OPENSSL
		PkgConfig::YYJSON
		howdy_opencv
)
install(TARGETS howdy RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

if(BUILD_TESTING)
	add_test(NAME native-howdy-help COMMAND howdy --help)
	add_test(NAME native-howdy-version COMMAND howdy version)
	add_test(NAME native-howdy-nonroot-path COMMAND howdy -U coverage-user list)
	howdy_expect_test_failure(native-howdy-nonroot-path)
	add_test(NAME native-howdy-empty COMMAND howdy)

	add_executable(howdy_dispatch_test tests/app/howdy_dispatch_test.cpp)
	howdy_configure_native_test_target(howdy_dispatch_test)
	target_link_libraries(
		howdy_dispatch_test
		PRIVATE
			howdy_cli
			PkgConfig::LIBCURL
			PkgConfig::OPENSSL
			PkgConfig::YYJSON
			howdy_opencv
	)
	add_test(NAME native-howdy-dispatch COMMAND howdy_dispatch_test)

	add_executable(howdy_main_test tests/app/howdy_main_test.cpp src/bin/howdy_main.cpp)
	howdy_configure_native_test_target(howdy_main_test)
	target_compile_definitions(
		howdy_main_test
		PRIVATE
			HOWDY_MAIN_ENTRYPOINT=howdy_main_test_entry
			HOWDY_MAIN_DISPATCH=howdy_main_test_dispatch
	)
	add_test(NAME native-howdy-main COMMAND howdy_main_test)

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
	)
	set_tests_properties(native-howdy-install-layout PROPERTIES RUN_SERIAL TRUE)
	add_test(
		NAME native-howdy-privileged-install-paths
		COMMAND
			sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/privileged_install_paths_test.sh"
			"${PROJECT_SOURCE_DIR}"
			"${PROJECT_BINARY_DIR}/privileged-install-path-cases"
	)
	set_tests_properties(native-howdy-privileged-install-paths PROPERTIES RUN_SERIAL TRUE)
endif()

set(config_path "${HOWDY_CONFIG_PATH}")
configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/completions/howdy.in"
	"${CMAKE_CURRENT_BINARY_DIR}/howdy-completion"
	@ONLY
)
install(
	FILES "${CMAKE_CURRENT_BINARY_DIR}/howdy-completion"
	DESTINATION "${CMAKE_INSTALL_DATADIR}/bash-completion/completions"
	RENAME howdy
	PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
)

install(
	DIRECTORY DESTINATION "${HOWDY_CONFIG_DIR}"
	DIRECTORY_PERMISSIONS
		OWNER_READ OWNER_WRITE OWNER_EXECUTE
		GROUP_READ GROUP_EXECUTE
)
configure_file(
	"${PROJECT_SOURCE_DIR}/cmake/install_config.cmake.in"
	"${PROJECT_BINARY_DIR}/cmake_install_config.cmake"
	@ONLY
)
install(SCRIPT "${PROJECT_BINARY_DIR}/cmake_install_config.cmake")
install(DIRECTORY DESTINATION "${HOWDY_MODELS_DIR}")
configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/howdy.1.in"
	"${CMAKE_CURRENT_BINARY_DIR}/howdy.1"
	@ONLY
)
configure_file(
	"${PROJECT_SOURCE_DIR}/pam/pam_howdy.8.in"
	"${CMAKE_CURRENT_BINARY_DIR}/pam_howdy.8"
	@ONLY
)
install(
	FILES "${CMAKE_CURRENT_BINARY_DIR}/howdy.1"
	DESTINATION "${CMAKE_INSTALL_MANDIR}/man1"
)
install(
	FILES "${CMAKE_CURRENT_BINARY_DIR}/pam_howdy.8"
	DESTINATION "${CMAKE_INSTALL_MANDIR}/man8"
)

howdy_set_test_timeouts()
