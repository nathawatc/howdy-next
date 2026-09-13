howdy_add_native_test(
	howdy_config_utils_test
	native-config-utils
	tests/config/config_utils_test.cpp
	tests/config/config_read_update_test.cpp
	tests/config/config_atomic_write_test.cpp
	tests/config/config_atomic_replace_test.cpp
	tests/config/config_path_security_test.cpp
)
target_link_libraries(howdy_config_utils_test PRIVATE howdy_config)

howdy_add_native_test(
	howdy_config_schema_test
	native-config-schema
	tests/config/config_schema_test.cpp
)
target_link_libraries(howdy_config_schema_test PRIVATE howdy_config_schema)

howdy_add_native_test(
	howdy_config_template_test
	native-config-template
	tests/config/config_template_test.cpp
)
target_link_libraries(
	howdy_config_template_test PRIVATE howdy_config_template howdy_config
)

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
		TIMEOUT 60
)

howdy_add_native_test(
	howdy_config_validation_test
	native-config-validation
	tests/config/config_validation_test.cpp
)
target_compile_definitions(
	howdy_config_validation_test
	PRIVATE "HOWDY_PACKAGED_CONFIG_PATH=\"${HOWDY_PACKAGED_CONFIG_PATH}\""
)
add_dependencies(howdy_config_validation_test howdy_packaged_config)
target_link_libraries(
	howdy_config_validation_test
	PRIVATE howdy_config
)

howdy_add_native_test(
	howdy_runtime_paths_test
	native-runtime-paths
	tests/config/runtime_paths_test.cpp
)
target_link_libraries(
	howdy_runtime_paths_test
	PRIVATE howdy_runtime_paths
)

howdy_add_native_test(
	howdy_runtime_config_load_test
	native-runtime-config-load
	tests/config/runtime_config_load_test.cpp
)
target_link_libraries(
	howdy_runtime_config_load_test
	PRIVATE howdy_runtime_config_loader howdy_runtime_paths
)

howdy_add_native_test(
	howdy_runtime_config_test
	native-runtime-config
	tests/config/runtime_config_test.cpp
)
target_link_libraries(
	howdy_runtime_config_test
	PRIVATE howdy_runtime_config_loader
)

howdy_add_native_test(
	howdy_config_reader_test
	native-config-reader
	tests/config/config_reader_test.cpp
)
target_link_libraries(
	howdy_config_reader_test
	PRIVATE howdy_config
)

howdy_add_native_test(
	howdy_runtime_config_defaults_test
	native-runtime-config-defaults
	tests/config/runtime_config_defaults_test.cpp
)
target_link_libraries(howdy_runtime_config_defaults_test PRIVATE howdy_runtime_config)
