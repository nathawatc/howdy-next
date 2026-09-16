cmake_minimum_required(VERSION 3.31)

project(howdy_cross_compilation_fixture LANGUAGES NONE)

if(NOT DEFINED HOWDY_SOURCE_DIR OR "${HOWDY_SOURCE_DIR}" STREQUAL "")
	message(FATAL_ERROR "HOWDY_SOURCE_DIR is required")
endif()
include("${HOWDY_SOURCE_DIR}/cmake/howdy_cross_compilation.cmake")

if(NOT CMAKE_CROSSCOMPILING)
	message(FATAL_ERROR "Fixture did not enter cross-compiling mode")
endif()
add_executable(howdy_config_generator IMPORTED)
howdy_set_crosscompiling_emulator(howdy_config_generator)
get_target_property(
	fixture_emulator
	howdy_config_generator
	CROSSCOMPILING_EMULATOR
)
if(NOT fixture_emulator STREQUAL CMAKE_CROSSCOMPILING_EMULATOR)
	message(
		FATAL_ERROR
		"Fixture emulator property mismatch: ${fixture_emulator}"
	)
endif()
