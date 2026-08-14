install(SCRIPT "${PROJECT_BINARY_DIR}/cmake_install_prefix_guard.cmake")
add_library(howdy_config_schema STATIC src/config/config_schema.cpp)
howdy_configure_native_target(howdy_config_schema)

add_library(howdy_config_template STATIC src/config/config_template.cpp)
howdy_configure_native_target(howdy_config_template)
target_link_libraries(howdy_config_template PUBLIC howdy_config_schema)

add_library(
	howdy_config
	STATIC
	src/config/config_reader.cpp
	src/config/runtime_config.cpp
	src/config/config_utils.cpp
	src/config/config_validation.cpp
)
howdy_configure_native_target(howdy_config)
target_link_libraries(
	howdy_config
	PUBLIC
		howdy_config_schema
		PkgConfig::INIREADER
)

add_executable(howdy_config_generator src/tools/generate_config.cpp)
howdy_configure_native_target(howdy_config_generator)
target_link_libraries(howdy_config_generator PRIVATE howdy_config_template)
if(CMAKE_CROSSCOMPILING)
	set_property(
		TARGET howdy_config_generator
		PROPERTY CROSSCOMPILING_EMULATOR "${CMAKE_CROSSCOMPILING_EMULATOR}"
	)
endif()

add_library(howdy_man_reference STATIC src/docs/man_reference.cpp)
howdy_configure_native_target(howdy_man_reference)
target_link_libraries(howdy_man_reference PUBLIC howdy_command_catalog)
target_include_directories(
	howdy_man_reference
	PUBLIC "${PROJECT_SOURCE_DIR}/pam/include"
)

add_executable(howdy_docs_generator src/tools/generate_docs.cpp)
howdy_configure_native_target(howdy_docs_generator)
target_include_directories(
	howdy_docs_generator
	PRIVATE "${PROJECT_SOURCE_DIR}/pam/include"
)
target_link_libraries(
	howdy_docs_generator
	PRIVATE howdy_command_catalog howdy_pam_option_catalog howdy_man_reference
)
if(CMAKE_CROSSCOMPILING)
	set_property(
		TARGET howdy_docs_generator
		PROPERTY CROSSCOMPILING_EMULATOR "${CMAKE_CROSSCOMPILING_EMULATOR}"
	)
endif()

set(
	HOWDY_PACKAGED_CONFIG_CANDIDATE_PATH
	"${HOWDY_PACKAGED_CONFIG_PATH}.candidate"
)
set(
	HOWDY_PACKAGED_CONFIG_SCRIPT
	"${PROJECT_SOURCE_DIR}/cmake/packaged_config.cmake"
)
add_custom_command(
	OUTPUT "${HOWDY_PACKAGED_CONFIG_PATH}"
	COMMAND
		"${CMAKE_COMMAND}"
		"-DHOWDY_PACKAGED_CONFIG_ACTION=prepare"
		"-DHOWDY_PACKAGED_CONFIG_CANDIDATE=${HOWDY_PACKAGED_CONFIG_CANDIDATE_PATH}"
		"-DHOWDY_PACKAGED_CONFIG_OUTPUT=${HOWDY_PACKAGED_CONFIG_PATH}"
		-P "${HOWDY_PACKAGED_CONFIG_SCRIPT}"
	COMMAND
		howdy_config_generator
		--output "${HOWDY_PACKAGED_CONFIG_CANDIDATE_PATH}"
	COMMAND
		"${CMAKE_COMMAND}"
		"-DHOWDY_PACKAGED_CONFIG_ACTION=publish"
		"-DHOWDY_PACKAGED_CONFIG_CANDIDATE=${HOWDY_PACKAGED_CONFIG_CANDIDATE_PATH}"
		"-DHOWDY_PACKAGED_CONFIG_OUTPUT=${HOWDY_PACKAGED_CONFIG_PATH}"
		-P "${HOWDY_PACKAGED_CONFIG_SCRIPT}"
	DEPENDS
		howdy_config_generator
		"${HOWDY_PACKAGED_CONFIG_SCRIPT}"
	COMMENT "Generating packaged Howdy config.ini"
	VERBATIM
)
add_custom_target(
	howdy_packaged_config ALL
	DEPENDS "${HOWDY_PACKAGED_CONFIG_PATH}"
)

add_library(
	howdy_runtime_paths
	STATIC
	src/config/runtime_paths.cpp
	src/config/runtime_config_defaults.cpp
)
howdy_configure_native_target(howdy_runtime_paths)
target_link_libraries(howdy_runtime_paths PUBLIC howdy_config)

add_library(
	howdy_user_model_readiness STATIC
	src/storage/user_model_readiness.cpp
)
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
target_link_libraries(
	howdy_preview_engine
	PUBLIC howdy_opencv
	PRIVATE howdy_frame_processing
)

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

add_library(
	howdy_compare_capture_session STATIC
	src/compare/capture_session.cpp
)
howdy_configure_native_target(howdy_compare_capture_session)
target_link_libraries(
	howdy_compare_capture_session
	PUBLIC howdy_opencv
	PRIVATE howdy_vision_core
)

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

add_library(howdy_face_model STATIC src/vision/face_model.cpp)
howdy_configure_native_target(howdy_face_model)
target_link_libraries(
	howdy_face_model
	PUBLIC howdy_opencv howdy_config
	PRIVATE howdy_runtime_paths howdy_model_file howdy_vision_core
)

add_library(
	howdy_user_models
	STATIC
	src/storage/user_model_store.cpp
	src/storage/user_models.cpp
)
howdy_configure_native_target(howdy_user_models)
target_link_libraries(
	howdy_user_models
	PRIVATE howdy_runtime_paths howdy_user_model_readiness howdy_user_model_codec
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
		howdy_face_model
		howdy_user_models
		howdy_runtime_paths
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
set_target_properties(
	howdy_auth_helper PROPERTIES OUTPUT_NAME "howdy-auth-helper"
)
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
		howdy_command_catalog
		howdy_face_model
		howdy_user_models
		howdy_runtime_paths
		howdy_test_preview
		howdy_preview_engine
		howdy_frame_processing
		howdy_compare_logic
		PkgConfig::LIBCURL
		PkgConfig::OPENSSL
		howdy_opencv
)

if(BUILD_TESTING)
	add_executable(howdy_command_catalog_test tests/app/command_catalog_test.cpp)
	howdy_configure_native_test_target(howdy_command_catalog_test)
	target_link_libraries(howdy_command_catalog_test PRIVATE howdy_command_catalog)
	add_test(NAME native-command-catalog COMMAND howdy_command_catalog_test)

	add_executable(howdy_man_reference_test tests/docs/man_reference_test.cpp)
	howdy_configure_native_test_target(howdy_man_reference_test)
	target_link_libraries(
		howdy_man_reference_test
		PRIVATE howdy_man_reference howdy_command_catalog howdy_pam_option_catalog
	)
	add_test(NAME native-man-reference COMMAND howdy_man_reference_test)

	add_test(NAME native-compare-help COMMAND howdy_compare --help)
	add_test(NAME native-auth-helper-help COMMAND howdy_auth_helper --help)
	add_test(NAME native-auth-helper-empty COMMAND howdy_auth_helper)
	howdy_expect_test_failure(native-auth-helper-empty)
	add_test(
		NAME native-auth-helper-unknown-command
		COMMAND howdy_auth_helper unknown alice
	)
	howdy_expect_test_failure(native-auth-helper-unknown-command)
	add_test(
		NAME native-auth-helper-prepare-guard
		COMMAND howdy_auth_helper prepare ../alice
	)
	howdy_expect_test_failure(native-auth-helper-prepare-guard)
	add_test(
		NAME native-auth-helper-cleanup-guard
		COMMAND howdy_auth_helper cleanup /tmp/not-howdy-runtime
	)
	howdy_expect_test_failure(native-auth-helper-cleanup-guard)

	add_executable(
		howdy_auth_helper_test
		tests/auth_helper/auth_helper_test.cpp
		tests/auth_helper/auth_helper_path_test.cpp
		tests/auth_helper/auth_helper_staging_test.cpp
	)
	howdy_configure_native_test_target(howdy_auth_helper_test)
	target_link_libraries(howdy_auth_helper_test PRIVATE howdy_auth_helper_core)
	add_test(NAME native-auth-helper COMMAND howdy_auth_helper_test)

	add_test(NAME native-compare-unknown-arg COMMAND howdy_compare --unknown)
	howdy_expect_test_failure(native-compare-unknown-arg)
	add_test(NAME native-compare-missing-user COMMAND howdy_compare)
	howdy_expect_test_failure(native-compare-missing-user)
	add_test(NAME native-compare-no-model COMMAND howdy_compare coverage-user)
	set(
		compare_test_models_empty
		"${CMAKE_CURRENT_BINARY_DIR}/compare-test-models-empty"
	)
	set_tests_properties(
		native-compare-no-model
		PROPERTIES
			WILL_FAIL TRUE
			ENVIRONMENT "HOWDY_USER_MODELS_DIR=${compare_test_models_empty}"
	)

	add_executable(howdy_compare_args_test tests/compare/compare_args_test.cpp)
	howdy_configure_native_test_target(howdy_compare_args_test)
	target_link_libraries(howdy_compare_args_test PRIVATE howdy_compare_args)
	add_test(NAME native-compare-args COMMAND howdy_compare_args_test)

	add_executable(howdy_compare_logic_test tests/compare/compare_logic_test.cpp)
	howdy_configure_native_test_target(howdy_compare_logic_test)
	target_link_libraries(
		howdy_compare_logic_test PRIVATE howdy_compare_logic howdy_opencv
	)
	add_test(NAME native-compare-logic COMMAND howdy_compare_logic_test)

	add_executable(
		howdy_frame_processing_test tests/vision/frame_processing_test.cpp
	)
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
		PRIVATE howdy_compare_capture_session
	)
	add_test(
		NAME native-compare-capture-session
		COMMAND howdy_compare_capture_session_test
	)

	add_executable(
		howdy_compare_privileges_test tests/compare/compare_privileges_test.cpp
	)
	howdy_configure_native_test_target(howdy_compare_privileges_test)
	target_link_libraries(
		howdy_compare_privileges_test PRIVATE howdy_compare_privileges
	)
	add_test(NAME native-compare-privileges COMMAND howdy_compare_privileges_test)

	add_executable(
		howdy_compare_processing_test tests/compare/compare_processing_test.cpp
	)
	howdy_configure_native_test_target(howdy_compare_processing_test)
	target_link_libraries(
		howdy_compare_processing_test PRIVATE howdy_compare_processing
	)
	add_test(NAME native-compare-processing COMMAND howdy_compare_processing_test)

	add_executable(
		howdy_compare_sandbox_test tests/compare/compare_sandbox_test.cpp
	)
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
	target_link_libraries(
		howdy_face_model_test
		PRIVATE
			howdy_face_model
			howdy_runtime_paths
	)
	add_test(NAME native-face-model COMMAND howdy_face_model_test)

	add_executable(howdy_config_utils_test tests/config/config_utils_test.cpp)
	howdy_configure_native_test_target(howdy_config_utils_test)
	target_link_libraries(howdy_config_utils_test PRIVATE howdy_config)
	add_test(NAME native-config-utils COMMAND howdy_config_utils_test)

	add_executable(
		howdy_config_schema_test tests/config/config_schema_test.cpp
	)
	howdy_configure_native_test_target(howdy_config_schema_test)
	target_link_libraries(howdy_config_schema_test PRIVATE howdy_config_schema)
	add_test(NAME native-config-schema COMMAND howdy_config_schema_test)

	add_executable(
		howdy_config_template_test tests/config/config_template_test.cpp
	)
	howdy_configure_native_test_target(howdy_config_template_test)
	target_link_libraries(
		howdy_config_template_test PRIVATE howdy_config_template
	)
	add_test(NAME native-config-template COMMAND howdy_config_template_test)

	add_executable(
		howdy_config_generator_test tests/config/config_generator_test.cpp
	)
	howdy_configure_native_test_target(howdy_config_generator_test)
	add_dependencies(howdy_config_generator_test howdy_config_generator)

	if(NOT CMAKE_CROSSCOMPILING)
		add_test(
			NAME native-config-generator
			COMMAND howdy_config_generator_test "$<TARGET_FILE:howdy_config_generator>"
		)
		add_test(
			NAME native-config-cross-compilation
			COMMAND
				"${CMAKE_COMMAND}"
				"-DHOWDY_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
				"-DHOWDY_TEST_ROOT=${PROJECT_BINARY_DIR}/config-cross-compilation-test"
				"-DHOWDY_CMAKE_GENERATOR=${CMAKE_GENERATOR}"
				"-DHOWDY_PAM_INCLUDE_DIR=${PAM_INCLUDE_DIR}"
				"-DHOWDY_PAM_LIBRARY=${PAM_LIBRARY}"
				-P "${PROJECT_SOURCE_DIR}/tests/cmake/config_generation_test.cmake"
		)
		set_tests_properties(
			native-config-cross-compilation
			PROPERTIES
				RUN_SERIAL TRUE
				TIMEOUT 120
		)
	endif()
	add_test(
		NAME native-version-metadata
		COMMAND
			"${CMAKE_COMMAND}"
			"-DHOWDY_VERSION_GENERATOR=${HOWDY_VERSION_GENERATOR}"
			"-DHOWDY_VERSION_TEMPLATE=${HOWDY_VERSION_TEMPLATE}"
			"-DHOWDY_PROJECT_VERSION=${PROJECT_VERSION}"
			"-DHOWDY_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
			"-DHOWDY_TEST_ROOT=${PROJECT_BINARY_DIR}/version-metadata-test"
			"-DHOWDY_GIT_EXECUTABLE=${HOWDY_GIT_EXECUTABLE}"
			-P "${PROJECT_SOURCE_DIR}/tests/cmake/version_metadata_test.cmake"
	)
	set_tests_properties(
		native-version-metadata
		PROPERTIES
			RUN_SERIAL TRUE
			TIMEOUT 60
	)

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

	add_executable(
		howdy_model_provenance_test
		tests/model_assets/opencv_model_provenance_test.cpp
	)
	howdy_configure_native_test_target(howdy_model_provenance_test)
	target_compile_definitions(
		howdy_model_provenance_test
		PRIVATE "HOWDY_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\""
	)
	add_test(NAME native-model-provenance COMMAND howdy_model_provenance_test)

	add_executable(
		howdy_capture_device_path_test tests/vision/capture_device_path_test.cpp
	)
	howdy_configure_native_test_target(howdy_capture_device_path_test)
	add_test(
		NAME native-capture-device-path COMMAND howdy_capture_device_path_test
	)

	add_executable(howdy_video_capture_test tests/vision/video_capture_test.cpp)
	howdy_configure_native_test_target(howdy_video_capture_test)
	target_link_libraries(howdy_video_capture_test PRIVATE howdy_vision_core)
	add_test(NAME native-video-capture COMMAND howdy_video_capture_test)

	add_executable(
		howdy_enrollment_capture_test tests/cli/enrollment_capture_test.cpp
	)
	howdy_configure_native_test_target(howdy_enrollment_capture_test)
	target_link_libraries(
		howdy_enrollment_capture_test
		PRIVATE howdy_frame_processing howdy_compare_logic howdy_opencv
	)
	add_test(NAME native-enrollment-capture COMMAND howdy_enrollment_capture_test)

	add_executable(
		howdy_config_validation_test tests/config/config_validation_test.cpp
	)
	howdy_configure_native_test_target(howdy_config_validation_test)
	target_compile_definitions(
		howdy_config_validation_test
		PRIVATE "HOWDY_PACKAGED_CONFIG_PATH=\"${HOWDY_PACKAGED_CONFIG_PATH}\""
	)
	add_dependencies(howdy_config_validation_test howdy_packaged_config)
	target_link_libraries(
		howdy_config_validation_test
		PRIVATE howdy_config
	)
	add_test(NAME native-config-validation COMMAND howdy_config_validation_test)

	add_executable(
		howdy_user_models_test
		tests/storage/user_models_test.cpp
		tests/storage/user_models_mutation_test.cpp
		tests/storage/user_models_failure_test.cpp
	)
	howdy_configure_native_test_target(howdy_user_models_test)
	target_link_libraries(
		howdy_user_models_test
		PRIVATE
			howdy_user_models
			howdy_user_model_readiness
	)
	add_test(NAME native-user-models COMMAND howdy_user_models_test)

	add_executable(
		howdy_user_model_codec_test tests/storage/user_model_codec_test.cpp
	)
	howdy_configure_native_test_target(howdy_user_model_codec_test)
	target_link_libraries(
		howdy_user_model_codec_test PRIVATE howdy_user_model_codec
	)
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

	add_executable(
		howdy_test_preview_session_test tests/cli/test_preview_session_test.cpp
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

	add_executable(howdy_preview_engine_test tests/vision/preview_engine_test.cpp)
	howdy_configure_native_test_target(howdy_preview_engine_test)
	target_link_libraries(
		howdy_preview_engine_test
		PRIVATE howdy_preview_engine
	)
	add_test(NAME native-preview-engine COMMAND howdy_preview_engine_test)

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
		PRIVATE howdy_runtime_paths
	)
	add_test(NAME native-runtime-paths COMMAND howdy_runtime_paths_test)

	add_executable(
		howdy_runtime_config_load_test tests/config/runtime_config_load_test.cpp
	)
	howdy_configure_native_test_target(howdy_runtime_config_load_test)
	target_link_libraries(
		howdy_runtime_config_load_test
		PRIVATE howdy_runtime_paths
	)
	add_test(
		NAME native-runtime-config-load COMMAND howdy_runtime_config_load_test
	)
	set_tests_properties(
		native-runtime-config-load
		PROPERTIES ENVIRONMENT "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}"
	)

	add_executable(howdy_runtime_config_test tests/config/runtime_config_test.cpp)
	howdy_configure_native_test_target(howdy_runtime_config_test)
	target_link_libraries(
		howdy_runtime_config_test
		PRIVATE howdy_config
	)
	add_test(NAME native-runtime-config COMMAND howdy_runtime_config_test)
	set_tests_properties(
		native-runtime-config
		PROPERTIES ENVIRONMENT "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}"
	)

	add_executable(
		howdy_invoking_user_env_test tests/support/invoking_user_env_test.cpp
	)
	howdy_configure_native_test_target(howdy_invoking_user_env_test)
	add_test(NAME native-invoking-user-env COMMAND howdy_invoking_user_env_test)

	add_executable(howdy_config_reader_test tests/config/config_reader_test.cpp)
	howdy_configure_native_test_target(howdy_config_reader_test)
	target_link_libraries(
		howdy_config_reader_test
		PRIVATE howdy_config
	)
	add_test(NAME native-config-reader COMMAND howdy_config_reader_test)
endif()

add_executable(howdy src/bin/howdy_main.cpp)
howdy_configure_native_target(howdy)
target_link_libraries(howdy PRIVATE howdy_cli)
install(TARGETS howdy RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

set(HOWDY_DOC_REFERENCE_DIR "${PROJECT_BINARY_DIR}/generated/docs")
set(
	HOWDY_COMMAND_REFERENCE_PATH
	"${HOWDY_DOC_REFERENCE_DIR}/howdy-commands.roff"
)
set(HOWDY_OPTION_REFERENCE_PATH "${HOWDY_DOC_REFERENCE_DIR}/howdy-options.roff")
set(
	HOWDY_WORKAROUND_REFERENCE_PATH
	"${HOWDY_DOC_REFERENCE_DIR}/pam-workarounds.roff"
)
set(HOWDY_MAN1_PATH "${CMAKE_CURRENT_BINARY_DIR}/howdy.1")
set(HOWDY_MAN8_PATH "${CMAKE_CURRENT_BINARY_DIR}/pam_howdy.8")
set(HOWDY_MAN_COMPOSE_SCRIPT "${PROJECT_SOURCE_DIR}/cmake/compose_man.cmake")

add_custom_command(
	OUTPUT
		"${HOWDY_COMMAND_REFERENCE_PATH}"
		"${HOWDY_OPTION_REFERENCE_PATH}"
		"${HOWDY_WORKAROUND_REFERENCE_PATH}"
		"${HOWDY_MAN1_PATH}"
		"${HOWDY_MAN8_PATH}"
	COMMAND
		howdy_docs_generator
		--output-dir "${HOWDY_DOC_REFERENCE_DIR}"
	COMMAND
		"${CMAKE_COMMAND}"
		"-DMAN1_TEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/howdy.1.in"
		"-DMAN1_OUTPUT_PATH=${HOWDY_MAN1_PATH}"
		"-DMAN8_TEMPLATE=${PROJECT_SOURCE_DIR}/pam/pam_howdy.8.in"
		"-DMAN8_OUTPUT_PATH=${HOWDY_MAN8_PATH}"
		"-DCOMMAND_REFERENCE=${HOWDY_COMMAND_REFERENCE_PATH}"
		"-DOPTION_REFERENCE=${HOWDY_OPTION_REFERENCE_PATH}"
		"-DWORKAROUND_REFERENCE=${HOWDY_WORKAROUND_REFERENCE_PATH}"
		"-DPROJECT_VERSION=${PROJECT_VERSION}"
		"-DHOWDY_MAN_DATE=${HOWDY_MAN_DATE}"
		"-DHOWDY_CONFIG_DIR=${HOWDY_CONFIG_DIR}"
		"-DHOWDY_CONFIG_PATH=${HOWDY_CONFIG_PATH}"
		"-DHOWDY_MODELS_DIR=${HOWDY_MODELS_DIR}"
		"-DHOWDY_USER_MODELS_DIR=${HOWDY_USER_MODELS_DIR}"
		"-DHOWDY_AUTH_HELPER_PATH=${HOWDY_AUTH_HELPER_PATH}"
		-P "${HOWDY_MAN_COMPOSE_SCRIPT}"
	DEPENDS
		howdy_docs_generator
		"${CMAKE_CURRENT_SOURCE_DIR}/howdy.1.in"
		"${PROJECT_SOURCE_DIR}/pam/pam_howdy.8.in"
		"${PROJECT_SOURCE_DIR}/howdy/include/app/command_catalog.hpp"
		"${PROJECT_SOURCE_DIR}/howdy/src/app/command_catalog.cpp"
		"${PROJECT_SOURCE_DIR}/howdy/include/docs/man_reference.hpp"
		"${PROJECT_SOURCE_DIR}/howdy/src/docs/man_reference.cpp"
		"${PROJECT_SOURCE_DIR}/howdy/src/tools/generate_docs.cpp"
		"${PROJECT_SOURCE_DIR}/pam/include/module/pam_option_catalog.hpp"
		"${PROJECT_SOURCE_DIR}/pam/src/module/pam_option_catalog.cpp"
		"${HOWDY_MAN_COMPOSE_SCRIPT}"
	COMMENT "Generating and composing Howdy manual pages"
	VERBATIM
)
add_custom_target(
	howdy_docs ALL DEPENDS "${HOWDY_MAN1_PATH}" "${HOWDY_MAN8_PATH}"
)

if(BUILD_TESTING)
	set(howdy_integration_dir "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration")
	add_test(NAME native-howdy-help COMMAND howdy --help)
	add_test(NAME native-howdy-version COMMAND howdy version)
	add_test(NAME native-howdy-nonroot-path COMMAND howdy -U coverage-user list)
	howdy_expect_test_failure(native-howdy-nonroot-path)
	add_test(NAME native-howdy-empty COMMAND howdy)
	add_test(
		NAME native-howdy-bash-completion
		COMMAND
			bash
			"${howdy_integration_dir}/bash_completion_test.sh"
			"$<TARGET_FILE:howdy>"
			"${CMAKE_CURRENT_SOURCE_DIR}/completions/howdy"
	)

	add_executable(howdy_dispatch_test tests/app/howdy_dispatch_test.cpp)
	howdy_configure_native_test_target(howdy_dispatch_test)
	target_link_libraries(howdy_dispatch_test PRIVATE howdy_cli)
	add_test(NAME native-howdy-dispatch COMMAND howdy_dispatch_test)

	add_executable(
		howdy_main_test tests/app/howdy_main_test.cpp src/bin/howdy_main.cpp
	)
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
			"${HOWDY_LICENSES_INSTALL_DIR}"
	)
	set_tests_properties(native-howdy-install-layout PROPERTIES RUN_SERIAL TRUE)
	add_test(
		NAME native-howdy-privileged-install-paths
		COMMAND
			sh
			"${howdy_integration_dir}/privileged_install_paths_test.sh"
			"${PROJECT_SOURCE_DIR}"
			"${PROJECT_BINARY_DIR}/privileged-install-path-cases"
	)
	set_tests_properties(
		native-howdy-privileged-install-paths PROPERTIES RUN_SERIAL TRUE
	)
	if(NOT CMAKE_CROSSCOMPILING)
		add_test(
			NAME native-howdy-docs-generator
			COMMAND
				sh
				"${howdy_integration_dir}/docs_generator_test.sh"
				"$<TARGET_FILE:howdy_docs_generator>"
				"${PROJECT_BINARY_DIR}/docs-generator-test"
		)
	endif()
	add_test(
		NAME native-howdy-docs
		COMMAND
			sh
			"${howdy_integration_dir}/docs_test.sh"
			"${PROJECT_BINARY_DIR}"
			"${PROJECT_SOURCE_DIR}"
			"${HOWDY_MAN1_PATH}"
			"${HOWDY_MAN8_PATH}"
			"${HOWDY_CONFIG_DIR}"
			"${HOWDY_CONFIG_PATH}"
			"${HOWDY_MODELS_DIR}"
			"${HOWDY_USER_MODELS_DIR}"
			"${HOWDY_AUTH_HELPER_PATH}"
	)
endif()

install(
	FILES "${CMAKE_CURRENT_SOURCE_DIR}/completions/howdy"
	DESTINATION "${CMAKE_INSTALL_DATADIR}/bash-completion/completions"
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
	FILES "${HOWDY_MAN1_PATH}"
	DESTINATION "${CMAKE_INSTALL_MANDIR}/man1"
)
install(
	FILES "${HOWDY_MAN8_PATH}"
	DESTINATION "${CMAKE_INSTALL_MANDIR}/man8"
)

howdy_set_test_timeouts()
