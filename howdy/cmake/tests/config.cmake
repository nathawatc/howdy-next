add_executable(
	howdy_config_utils_test
	tests/config/config_utils_test.cpp
	tests/config/config_read_update_test.cpp
	tests/config/config_atomic_write_test.cpp
	tests/config/config_atomic_replace_test.cpp
	tests/config/config_path_security_test.cpp
)
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
	howdy_config_template_test PRIVATE howdy_config_template howdy_config
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

add_executable(howdy_config_reader_test tests/config/config_reader_test.cpp)
howdy_configure_native_test_target(howdy_config_reader_test)
target_link_libraries(
	howdy_config_reader_test
	PRIVATE howdy_config
)
add_test(NAME native-config-reader COMMAND howdy_config_reader_test)
