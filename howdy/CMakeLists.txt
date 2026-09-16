install(SCRIPT "${PROJECT_BINARY_DIR}/cmake_install_prefix_guard.cmake")

add_library(howdy_config_schema STATIC
	src/config/config_schema.cpp
	src/config/config_schema/internal.hpp
	src/config/config_schema/validation.cpp
)
howdy_configure_native_target(howdy_config_schema)

add_library(howdy_config_template STATIC src/config/config_template.cpp)
howdy_configure_native_target(howdy_config_template)
target_link_libraries(howdy_config_template PUBLIC howdy_config_schema)

add_library(howdy_runtime_config STATIC src/config/runtime_config.cpp)
howdy_configure_native_target(howdy_runtime_config)
target_link_libraries(howdy_runtime_config PUBLIC howdy_config_schema)

add_library(
	howdy_config
	STATIC
	src/config/config_reader.cpp
	src/config/config_reader/internal.hpp
	src/config/config_utils.cpp
	src/config/config_utils/internal.hpp
	src/config/config_utils/file_ops.cpp
	src/config/config_utils/key_file.cpp
	src/config/config_validation.cpp
	src/config/config_validation/internal.hpp
)
howdy_configure_native_target(howdy_config)
target_link_libraries(
	howdy_config
	PRIVATE
		howdy_config_schema
		PkgConfig::INIREADER
)

add_executable(howdy_config_generator src/tools/generate_config.cpp)
howdy_configure_native_target(howdy_config_generator)
target_link_libraries(howdy_config_generator PRIVATE howdy_config_template)
howdy_set_crosscompiling_emulator(howdy_config_generator)

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

add_library(howdy_runtime_paths STATIC src/config/runtime_paths.cpp)
howdy_configure_native_target(howdy_runtime_paths)

add_library(
	howdy_runtime_config_loader STATIC
	src/config/runtime_config_loader.cpp
	src/config/runtime_config_loader/internal.hpp
	src/config/runtime_config_loader/defaults.cpp
)
howdy_configure_native_target(howdy_runtime_config_loader)
target_link_libraries(
	howdy_runtime_config_loader
	PUBLIC howdy_runtime_config
	PRIVATE howdy_config howdy_runtime_paths PkgConfig::INIREADER
)

add_library(
	howdy_user_model_readiness STATIC
	src/storage/user_model_readiness.cpp
	src/storage/user_model_readiness/internal.hpp
	src/storage/user_model_readiness/staged.cpp
)
howdy_configure_native_target(howdy_user_model_readiness)
target_link_libraries(howdy_user_model_readiness PRIVATE PkgConfig::LIBACL)

add_library(howdy_model_file STATIC src/model_assets/model_file.cpp)
howdy_configure_native_target(howdy_model_file)

add_library(
	howdy_vision_core
	STATIC
	src/vision/face_detection.cpp
	src/vision/face_encoding.cpp
	src/vision/video_capture.cpp
)
howdy_configure_native_target(howdy_vision_core)
target_link_libraries(howdy_vision_core PUBLIC howdy_opencv
                      howdy_runtime_config)

add_library(howdy_frame_processing STATIC src/vision/frame_processing.cpp)
howdy_configure_native_target(howdy_frame_processing)
target_link_libraries(howdy_frame_processing PUBLIC howdy_opencv
                      howdy_runtime_config)

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
	src/cli/test/preview_session.cpp
	src/cli/test/preview_renderer.cpp
)
howdy_configure_native_target(howdy_test_preview)
target_link_libraries(
	howdy_test_preview
	PUBLIC
		howdy_preview_engine
		howdy_frame_processing
		howdy_opencv
	PRIVATE
		howdy_vision_core
)

add_library(howdy_user_model_codec STATIC
	src/storage/user_model_codec.cpp
	src/storage/user_model_codec/internal.hpp
	src/storage/user_model_codec/parsing.cpp
)
howdy_configure_native_target(howdy_user_model_codec)
target_link_libraries(howdy_user_model_codec PRIVATE PkgConfig::YYJSON)

add_library(howdy_face_model STATIC src/vision/face_model.cpp)
howdy_configure_native_target(howdy_face_model)
target_link_libraries(
	howdy_face_model
	PUBLIC howdy_opencv howdy_runtime_config
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

add_library(howdy_compare_args STATIC src/compare/args.cpp)
howdy_configure_native_target(howdy_compare_args)

add_library(howdy_compare_logic STATIC src/compare/logic.cpp)
howdy_configure_native_target(howdy_compare_logic)
target_link_libraries(howdy_compare_logic PUBLIC howdy_opencv)

add_library(howdy_compare_engine STATIC src/compare/engine.cpp)
howdy_configure_native_target(howdy_compare_engine)
target_link_libraries(
	howdy_compare_engine
	PUBLIC
		howdy_opencv
		howdy_runtime_config
	PRIVATE
		howdy_frame_processing
		howdy_compare_logic
)

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

add_library(
	howdy_compare_privileges STATIC
	src/compare/privileges.cpp
	src/compare/privileges/internal.hpp
	src/compare/privileges/operations.cpp
	src/compare/privileges/verification.cpp
)
howdy_configure_native_target(howdy_compare_privileges)

add_library(howdy_compare_sandbox STATIC src/compare/sandbox.cpp)
howdy_configure_native_target(howdy_compare_sandbox)

add_library(howdy_compare_processing STATIC src/compare/processing.cpp)
howdy_configure_native_target(howdy_compare_processing)
target_link_libraries(howdy_compare_processing PUBLIC howdy_opencv)

add_executable(howdy_compare src/bin/compare.cpp)
howdy_configure_native_target(howdy_compare)
target_link_libraries(
	howdy_compare
	PRIVATE
		howdy_runtime_config_loader
		howdy_compare_args
		howdy_compare_processing
		howdy_compare_privileges
		howdy_compare_sandbox
		howdy_compare_capture_session
		howdy_face_model
		howdy_user_models
		howdy_runtime_paths
		howdy_compare_engine
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
	src/auth_helper/runtime/internal.hpp
	src/auth_helper/runtime/sources.cpp
	src/auth_helper/runtime/slots.cpp
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
if(HOWDY_INSTALL_AUTH_HELPER_SETUID)
	install(
		TARGETS howdy_auth_helper
		RUNTIME DESTINATION "${HOWDY_HELPER_DIR}"
		PERMISSIONS
			OWNER_READ OWNER_WRITE OWNER_EXECUTE SETUID
			GROUP_READ GROUP_EXECUTE
			WORLD_READ WORLD_EXECUTE
	)
else()
	install(TARGETS howdy_auth_helper RUNTIME DESTINATION "${HOWDY_HELPER_DIR}")
endif()

add_library(
	howdy_cli_core
	STATIC
	src/app/howdy.cpp
	src/app/howdy/cli.cpp
	src/app/howdy/cli/internal.hpp
	src/app/howdy/completion.cpp
	src/app/howdy/completion/internal.hpp
)
howdy_configure_native_target(howdy_cli_core)
target_link_libraries(
	howdy_cli_core
	PUBLIC howdy_command_catalog
	PRIVATE howdy_config_schema
)

add_library(
	howdy_cli
	STATIC
	src/app/howdy/production.cpp
	src/cli/add.cpp
	src/cli/add/workflow.cpp
	src/cli/clear.cpp
	src/cli/config.cpp
	src/cli/config/edit_session.cpp
	src/cli/disable.cpp
	src/cli/download_models.cpp
	src/cli/list.cpp
	src/cli/remove.cpp
	src/cli/set.cpp
	src/cli/snapshot.cpp
	src/cli/snapshot/workflow.cpp
	src/cli/test.cpp
)
howdy_configure_native_target(howdy_cli)
target_link_libraries(
	howdy_cli
	PUBLIC
		PkgConfig::LIBCURL
		howdy_opencv
	PRIVATE
		howdy_cli_core
		howdy_config
		howdy_runtime_config_loader
		howdy_face_model
		howdy_user_models
		howdy_runtime_paths
		howdy_test_preview
		howdy_preview_engine
		howdy_frame_processing
		howdy_compare_logic
		PkgConfig::OPENSSL
)

add_executable(howdy src/bin/howdy_main.cpp)
howdy_configure_native_target(howdy)
target_link_libraries(howdy PRIVATE howdy_cli)
install(TARGETS howdy RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

include(cmake/docs.cmake)

if(BUILD_TESTING)
	function(howdy_add_native_test target test_name)
		add_executable(${target} ${ARGN})
		howdy_configure_native_test_target(${target})
		add_test(NAME ${test_name} COMMAND ${target})
	endfunction()

	include(cmake/tests/app.cmake)
	include(cmake/tests/auth_helper.cmake)
	include(cmake/tests/compare.cmake)
	include(cmake/tests/config.cmake)
	include(cmake/tests/storage.cmake)
	include(cmake/tests/vision.cmake)
	include(cmake/tests/cli.cmake)
	include(cmake/tests/support.cmake)
	include(cmake/tests/integration.cmake)
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

howdy_set_test_timeouts()
