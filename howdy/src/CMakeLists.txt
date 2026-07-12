install(SCRIPT "${PROJECT_BINARY_DIR}/cmake_install_prefix_guard.cmake")
set(HOWDY_NATIVE_DEPENDENCIES
	PkgConfig::INIREADER
	PkgConfig::LIBCURL
	PkgConfig::YYJSON
	howdy_opencv
)

add_library(howdy_model_file STATIC model_file.cpp)
howdy_configure_native_target(howdy_model_file)

add_library(howdy_compare_logic STATIC compare_logic.cpp)
howdy_configure_native_target(howdy_compare_logic)
target_link_libraries(howdy_compare_logic PUBLIC howdy_opencv)

add_library(howdy_frame_processing STATIC frame_processing.cpp)
howdy_configure_native_target(howdy_frame_processing)
target_link_libraries(howdy_frame_processing PUBLIC howdy_opencv)

add_library(howdy_preview_engine STATIC preview_engine.cpp)
howdy_configure_native_target(howdy_preview_engine)
target_link_libraries(howdy_preview_engine PUBLIC howdy_opencv)

add_library(
	howdy_test_preview
	STATIC
	cli/test_preview_session.cpp
	cli/test_preview_renderer.cpp
)
howdy_configure_native_target(howdy_test_preview)
target_link_libraries(
	howdy_test_preview
	PUBLIC
		howdy_preview_engine
		howdy_frame_processing
		howdy_opencv
)

add_library(howdy_compare_engine STATIC compare_engine.cpp)
howdy_configure_native_target(howdy_compare_engine)
target_link_libraries(howdy_compare_engine PUBLIC howdy_opencv)

add_library(howdy_compare_capture_session STATIC compare_capture_session.cpp)
howdy_configure_native_target(howdy_compare_capture_session)
target_link_libraries(howdy_compare_capture_session PUBLIC howdy_opencv)

set(
	HOWDY_NATIVE_CPP_SOURCES
	config/config_reader.cpp
	config/runtime_config.cpp
	config/config_schema.cpp
	config/config_utils.cpp
	config/config_validation.cpp
	config/runtime_paths.cpp
	core/face_detection.cpp
	core/face_encoding.cpp
	core/face_model.cpp
	recorders/video_capture.cpp
	storage/user_model_codec.cpp
	storage/user_model_readiness.cpp
	storage/user_model_store.cpp
	storage/user_models.cpp
)
add_library(howdy_native_cpp STATIC ${HOWDY_NATIVE_CPP_SOURCES})
howdy_configure_native_target(howdy_native_cpp)
target_link_libraries(howdy_native_cpp PUBLIC howdy_model_file ${HOWDY_NATIVE_DEPENDENCIES})

add_executable(howdy_compare compare_args.cpp compare.cpp)
howdy_configure_native_target(howdy_compare)
target_link_libraries(
	howdy_compare
	PRIVATE
		howdy_compare_capture_session
		howdy_native_cpp
		howdy_compare_engine
		howdy_frame_processing
		howdy_compare_logic
		${HOWDY_NATIVE_DEPENDENCIES}
)
set_target_properties(howdy_compare PROPERTIES OUTPUT_NAME "howdy-compare")
install(
	TARGETS howdy_compare
	RUNTIME DESTINATION "${HOWDY_HELPER_DIR}"
)

add_executable(
	howdy_auth_helper
	auth_helper.cpp
	auth_helper_runtime.cpp
	config/runtime_paths.cpp
	storage/user_model_readiness.cpp
)
howdy_configure_native_target(howdy_auth_helper)
target_link_libraries(howdy_auth_helper PRIVATE PkgConfig::LIBACL)
set_target_properties(howdy_auth_helper PROPERTIES OUTPUT_NAME "howdy-auth-helper")
install(
	TARGETS howdy_auth_helper
	RUNTIME DESTINATION "${HOWDY_HELPER_DIR}"
	PERMISSIONS
		OWNER_READ OWNER_WRITE OWNER_EXECUTE SETUID
		GROUP_READ GROUP_EXECUTE
		WORLD_READ WORLD_EXECUTE
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
		auth_helper.cpp
		auth_helper_runtime.cpp
		config/runtime_paths.cpp
		storage/user_model_readiness.cpp
		tests/auth_helper_test.cpp
	)
	howdy_configure_native_target(howdy_auth_helper_test)
	target_link_libraries(howdy_auth_helper_test PRIVATE PkgConfig::LIBACL)
	target_compile_definitions(howdy_auth_helper_test PRIVATE HOWDY_AUTH_HELPER_TESTING)
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

	add_executable(howdy_compare_args_test compare_args.cpp tests/compare_args_test.cpp)
	howdy_configure_native_target(howdy_compare_args_test)
	target_link_libraries(howdy_compare_args_test PRIVATE ${HOWDY_NATIVE_DEPENDENCIES})
	add_test(NAME native-compare-args COMMAND howdy_compare_args_test)

	add_executable(howdy_compare_logic_test tests/compare_logic_test.cpp)
	howdy_configure_native_target(howdy_compare_logic_test)
	target_link_libraries(howdy_compare_logic_test PRIVATE howdy_compare_logic howdy_opencv)
	add_test(NAME native-compare-logic COMMAND howdy_compare_logic_test)

	add_executable(howdy_frame_processing_test tests/frame_processing_test.cpp)
	howdy_configure_native_target(howdy_frame_processing_test)
	target_link_libraries(
		howdy_frame_processing_test
		PRIVATE howdy_frame_processing howdy_compare_logic howdy_opencv
	)
	add_test(NAME native-frame-processing COMMAND howdy_frame_processing_test)

	add_executable(howdy_compare_engine_test tests/compare_engine_test.cpp)
	howdy_configure_native_target(howdy_compare_engine_test)
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
		tests/compare_capture_session_test.cpp
	)
	howdy_configure_native_target(howdy_compare_capture_session_test)
	target_link_libraries(
		howdy_compare_capture_session_test
		PRIVATE
			howdy_compare_capture_session
			howdy_native_cpp
			${HOWDY_NATIVE_DEPENDENCIES}
	)
	add_test(NAME native-compare-capture-session COMMAND howdy_compare_capture_session_test)

	add_executable(howdy_face_matching_test tests/face_matching_test.cpp)
	howdy_configure_native_target(howdy_face_matching_test)
	add_test(NAME native-face-matching COMMAND howdy_face_matching_test)

	add_executable(
		howdy_face_detection_test
		tests/face_detection_test.cpp
		core/face_detection.cpp
	)
	howdy_configure_native_target(howdy_face_detection_test)
	target_link_libraries(howdy_face_detection_test PRIVATE howdy_opencv)
	add_test(NAME native-face-detection COMMAND howdy_face_detection_test)

	add_executable(
		howdy_face_encoding_test
		tests/face_encoding_test.cpp
		core/face_encoding.cpp
	)
	howdy_configure_native_target(howdy_face_encoding_test)
	target_link_libraries(howdy_face_encoding_test PRIVATE howdy_opencv)
	add_test(NAME native-face-encoding COMMAND howdy_face_encoding_test)

	add_executable(howdy_face_model_test tests/face_model_test.cpp)
	howdy_configure_native_target(howdy_face_model_test)
	target_link_libraries(howdy_face_model_test PRIVATE howdy_native_cpp ${HOWDY_NATIVE_DEPENDENCIES})
	add_test(NAME native-face-model COMMAND howdy_face_model_test)

	add_executable(howdy_config_utils_test tests/config_utils_test.cpp)
	howdy_configure_native_target(howdy_config_utils_test)
	target_link_libraries(howdy_config_utils_test PRIVATE howdy_native_cpp ${HOWDY_NATIVE_DEPENDENCIES})
	add_test(NAME native-config-utils COMMAND howdy_config_utils_test)

	add_executable(howdy_file_security_test tests/file_security_test.cpp)
	howdy_configure_native_target(howdy_file_security_test)
	add_test(NAME native-file-security COMMAND howdy_file_security_test)

	add_executable(howdy_fd_io_test tests/fd_io_test.cpp)
	howdy_configure_native_target(howdy_fd_io_test)
	add_test(NAME native-fd-io COMMAND howdy_fd_io_test)

	add_executable(howdy_model_file_test tests/model_file_test.cpp)
	howdy_configure_native_target(howdy_model_file_test)
	target_link_libraries(howdy_model_file_test PRIVATE howdy_model_file)
	add_test(NAME native-model-file COMMAND howdy_model_file_test)

	add_executable(howdy_capture_device_path_test tests/capture_device_path_test.cpp)
	howdy_configure_native_target(howdy_capture_device_path_test)
	add_test(NAME native-capture-device-path COMMAND howdy_capture_device_path_test)

	add_executable(
		howdy_video_capture_test
		recorders/video_capture.cpp
		tests/video_capture_test.cpp
	)
	howdy_configure_native_target(howdy_video_capture_test)
	target_link_libraries(howdy_video_capture_test PRIVATE howdy_native_cpp howdy_opencv)
	add_test(NAME native-video-capture COMMAND howdy_video_capture_test)

	add_executable(howdy_enrollment_capture_test tests/enrollment_capture_test.cpp)
	howdy_configure_native_target(howdy_enrollment_capture_test)
	target_link_libraries(
		howdy_enrollment_capture_test
		PRIVATE howdy_frame_processing howdy_compare_logic howdy_opencv
	)
	add_test(NAME native-enrollment-capture COMMAND howdy_enrollment_capture_test)

	add_executable(howdy_config_validation_test tests/config_validation_test.cpp)
	howdy_configure_native_target(howdy_config_validation_test)
	target_compile_definitions(
		howdy_config_validation_test
		PRIVATE HOWDY_PACKAGED_CONFIG_PATH="${PROJECT_SOURCE_DIR}/config/config.ini"
	)
	target_link_libraries(
		howdy_config_validation_test
		PRIVATE howdy_native_cpp ${HOWDY_NATIVE_DEPENDENCIES}
	)
	add_test(NAME native-config-validation COMMAND howdy_config_validation_test)

	add_executable(howdy_user_models_test tests/user_models_test.cpp)
	howdy_configure_native_target(howdy_user_models_test)
	target_link_libraries(howdy_user_models_test PRIVATE howdy_native_cpp ${HOWDY_NATIVE_DEPENDENCIES})
	add_test(NAME native-user-models COMMAND howdy_user_models_test)

	add_executable(
		howdy_user_model_codec_test
		storage/user_model_codec.cpp
		tests/user_model_codec_test.cpp
	)
	howdy_configure_native_target(howdy_user_model_codec_test)
	target_link_libraries(howdy_user_model_codec_test PRIVATE PkgConfig::YYJSON)
	add_test(NAME native-user-model-codec COMMAND howdy_user_model_codec_test)

	add_executable(howdy_clear_cli_test cli/clear.cpp tests/clear_cli_test.cpp)
	howdy_configure_native_target(howdy_clear_cli_test)
	target_link_libraries(howdy_clear_cli_test PRIVATE howdy_native_cpp ${HOWDY_NATIVE_DEPENDENCIES})
	add_test(NAME native-clear-cli COMMAND howdy_clear_cli_test)

	add_executable(
		howdy_config_cli_test
		cli/config.cpp
		cli/config_edit_session.cpp
		tests/config_cli_test.cpp
	)
	howdy_configure_native_target(howdy_config_cli_test)
	target_link_options(howdy_config_cli_test PRIVATE -Wl,--wrap=initgroups)
	target_link_libraries(howdy_config_cli_test PRIVATE howdy_native_cpp ${HOWDY_NATIVE_DEPENDENCIES})
	add_test(NAME native-config-cli COMMAND howdy_config_cli_test)

	add_executable(howdy_disable_cli_test cli/disable.cpp tests/disable_cli_test.cpp)
	howdy_configure_native_target(howdy_disable_cli_test)
	target_link_libraries(howdy_disable_cli_test PRIVATE howdy_native_cpp ${HOWDY_NATIVE_DEPENDENCIES})
	add_test(NAME native-disable-cli COMMAND howdy_disable_cli_test)

	add_executable(howdy_set_cli_test cli/set.cpp tests/set_cli_test.cpp)
	howdy_configure_native_target(howdy_set_cli_test)
	target_link_libraries(
		howdy_set_cli_test
		PRIVATE howdy_native_cpp PkgConfig::INIREADER PkgConfig::YYJSON howdy_opencv
	)
	add_test(NAME native-set-cli COMMAND howdy_set_cli_test)

	add_executable(howdy_list_cli_test cli/list.cpp tests/list_cli_test.cpp)
	howdy_configure_native_target(howdy_list_cli_test)
	target_link_libraries(
		howdy_list_cli_test
		PRIVATE howdy_native_cpp PkgConfig::INIREADER PkgConfig::YYJSON howdy_opencv
	)
	add_test(NAME native-list-cli COMMAND howdy_list_cli_test)

	add_executable(howdy_add_cli_test cli/add.cpp tests/add_cli_test.cpp)
	howdy_configure_native_target(howdy_add_cli_test)
	target_link_libraries(
		howdy_add_cli_test
		PRIVATE
			howdy_native_cpp
			howdy_frame_processing
			howdy_compare_logic
			PkgConfig::INIREADER
			PkgConfig::YYJSON
			howdy_opencv
	)
	add_test(NAME native-add-cli COMMAND howdy_add_cli_test)

	add_executable(howdy_remove_cli_test cli/remove.cpp tests/remove_cli_test.cpp)
	howdy_configure_native_target(howdy_remove_cli_test)
	target_link_libraries(
		howdy_remove_cli_test
		PRIVATE howdy_native_cpp PkgConfig::INIREADER PkgConfig::YYJSON howdy_opencv
	)
	add_test(NAME native-remove-cli COMMAND howdy_remove_cli_test)

	add_executable(howdy_test_cli_test cli/test.cpp tests/test_cli_test.cpp)
	howdy_configure_native_target(howdy_test_cli_test)
	target_link_libraries(
		howdy_test_cli_test
		PRIVATE
			howdy_native_cpp
			howdy_test_preview
			howdy_preview_engine
			howdy_frame_processing
			PkgConfig::INIREADER
			PkgConfig::YYJSON
			howdy_opencv
	)
	add_test(NAME native-test-cli COMMAND howdy_test_cli_test)

	add_executable(
		howdy_test_preview_session_test
		cli/test.cpp
		tests/test_preview_session_test.cpp
	)
	howdy_configure_native_target(howdy_test_preview_session_test)
	target_link_libraries(
		howdy_test_preview_session_test
		PRIVATE
			howdy_native_cpp
			howdy_test_preview
			howdy_preview_engine
			howdy_frame_processing
			howdy_opencv
	)
	add_test(NAME native-test-preview-session COMMAND howdy_test_preview_session_test)

	add_executable(howdy_preview_engine_test tests/preview_engine_test.cpp)
	howdy_configure_native_target(howdy_preview_engine_test)
	target_link_libraries(
		howdy_preview_engine_test
		PRIVATE
			howdy_native_cpp
			howdy_preview_engine
			howdy_frame_processing
			howdy_opencv
	)
	add_test(NAME native-preview-engine COMMAND howdy_preview_engine_test)

	add_executable(howdy_snapshot_cli_test cli/snapshot.cpp tests/snapshot_cli_test.cpp)
	howdy_configure_native_target(howdy_snapshot_cli_test)
	target_link_libraries(
		howdy_snapshot_cli_test
		PRIVATE howdy_native_cpp ${HOWDY_NATIVE_DEPENDENCIES}
	)
	add_test(NAME native-snapshot-cli COMMAND howdy_snapshot_cli_test)

	add_executable(
		howdy_snapshot_writer_test
		cli/snapshot.cpp
		tests/snapshot_writer_test.cpp
	)
	howdy_configure_native_target(howdy_snapshot_writer_test)
	target_link_libraries(
		howdy_snapshot_writer_test
		PRIVATE howdy_native_cpp ${HOWDY_NATIVE_DEPENDENCIES}
	)
	add_test(NAME native-snapshot-writer COMMAND howdy_snapshot_writer_test)

	add_executable(
		howdy_download_models_test
		cli/download_models.cpp
		config/runtime_paths.cpp
		tests/download_models_test.cpp
	)
	howdy_configure_native_target(howdy_download_models_test)
	target_link_libraries(
		howdy_download_models_test
		PRIVATE howdy_model_file PkgConfig::LIBCURL PkgConfig::OPENSSL howdy_opencv
	)
	add_test(NAME native-download-models COMMAND howdy_download_models_test)

	add_executable(howdy_user_names_test tests/user_names_test.cpp)
	howdy_configure_native_target(howdy_user_names_test)
	add_test(NAME native-user-names COMMAND howdy_user_names_test)

	add_executable(howdy_runtime_paths_test tests/runtime_paths_test.cpp)
	howdy_configure_native_target(howdy_runtime_paths_test)
	target_link_libraries(
		howdy_runtime_paths_test
		PRIVATE howdy_native_cpp ${HOWDY_NATIVE_DEPENDENCIES}
	)
	add_test(NAME native-runtime-paths COMMAND howdy_runtime_paths_test)

	add_executable(howdy_runtime_config_load_test tests/runtime_config_load_test.cpp)
	howdy_configure_native_target(howdy_runtime_config_load_test)
	target_link_libraries(
		howdy_runtime_config_load_test
		PRIVATE howdy_native_cpp ${HOWDY_NATIVE_DEPENDENCIES}
	)
	add_test(NAME native-runtime-config-load COMMAND howdy_runtime_config_load_test)
	set_tests_properties(
		native-runtime-config-load
		PROPERTIES ENVIRONMENT "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}"
	)

	add_executable(howdy_runtime_config_test tests/runtime_config_test.cpp)
	howdy_configure_native_target(howdy_runtime_config_test)
	target_link_libraries(
		howdy_runtime_config_test
		PRIVATE howdy_native_cpp ${HOWDY_NATIVE_DEPENDENCIES}
	)
	add_test(NAME native-runtime-config COMMAND howdy_runtime_config_test)
	set_tests_properties(
		native-runtime-config
		PROPERTIES ENVIRONMENT "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}"
	)

	add_executable(howdy_invoking_user_env_test tests/invoking_user_env_test.cpp)
	howdy_configure_native_target(howdy_invoking_user_env_test)
	target_link_libraries(howdy_invoking_user_env_test PRIVATE ${HOWDY_NATIVE_DEPENDENCIES})
	add_test(NAME native-invoking-user-env COMMAND howdy_invoking_user_env_test)

	add_executable(howdy_config_reader_test tests/config_reader_test.cpp)
	howdy_configure_native_target(howdy_config_reader_test)
	target_link_libraries(
		howdy_config_reader_test
		PRIVATE howdy_native_cpp ${HOWDY_NATIVE_DEPENDENCIES}
	)
	add_test(NAME native-config-reader COMMAND howdy_config_reader_test)
endif()

add_library(
	howdy_cli
	STATIC
	howdy.cpp
	cli/add.cpp
	cli/clear.cpp
	cli/config.cpp
	cli/config_edit_session.cpp
	cli/disable.cpp
	cli/download_models.cpp
	cli/list.cpp
	cli/remove.cpp
	cli/set.cpp
	cli/snapshot.cpp
	cli/test.cpp
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
		PkgConfig::INIREADER
		PkgConfig::LIBCURL
		PkgConfig::OPENSSL
		PkgConfig::YYJSON
		howdy_opencv
)

add_executable(howdy howdy_main.cpp)
howdy_configure_native_target(howdy)
target_link_libraries(
	howdy
	PRIVATE
		howdy_cli
		PkgConfig::INIREADER
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

	add_executable(howdy_dispatch_test tests/howdy_dispatch_test.cpp)
	howdy_configure_native_target(howdy_dispatch_test)
	target_link_libraries(
		howdy_dispatch_test
		PRIVATE
			howdy_cli
			PkgConfig::INIREADER
			PkgConfig::LIBCURL
			PkgConfig::OPENSSL
			PkgConfig::YYJSON
			howdy_opencv
	)
	add_test(NAME native-howdy-dispatch COMMAND howdy_dispatch_test)

	add_test(
		NAME native-howdy-install-layout
		COMMAND
			sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/install_layout_test.sh"
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
endif()

set(config_path "${HOWDY_CONFIG_PATH}")
configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/autocomplete/howdy.in"
	"${CMAKE_CURRENT_BINARY_DIR}/autocomplete"
	@ONLY
)
install(
	FILES "${CMAKE_CURRENT_BINARY_DIR}/autocomplete"
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
install(
	FILES "${PROJECT_SOURCE_DIR}/howdy/howdy.1"
	DESTINATION "${CMAKE_INSTALL_MANDIR}/man1"
)
install(
	FILES "${PROJECT_SOURCE_DIR}/pam/pam_howdy.8"
	DESTINATION "${CMAKE_INSTALL_MANDIR}/man8"
)

howdy_set_test_timeouts()
