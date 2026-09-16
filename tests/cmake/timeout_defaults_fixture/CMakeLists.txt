cmake_minimum_required(VERSION 3.31)

project(howdy_timeout_defaults_fixture LANGUAGES NONE)

if(NOT DEFINED HOWDY_SOURCE_DIR OR "${HOWDY_SOURCE_DIR}" STREQUAL "")
	message(FATAL_ERROR "HOWDY_SOURCE_DIR is required")
endif()
include("${HOWDY_SOURCE_DIR}/cmake/howdy_test_timeouts.cmake")

enable_testing()
add_test(NAME default-timeout COMMAND "${CMAKE_COMMAND}" -E true)
add_test(NAME explicit-timeout COMMAND "${CMAKE_COMMAND}" -E true)
set_tests_properties(explicit-timeout PROPERTIES TIMEOUT 120)
howdy_set_test_timeouts()

get_test_property(default-timeout TIMEOUT default_timeout)
if(NOT default_timeout STREQUAL "30")
	message(FATAL_ERROR "Default timeout mismatch: ${default_timeout}")
endif()
get_test_property(explicit-timeout TIMEOUT explicit_timeout)
if(NOT explicit_timeout STREQUAL "120")
	message(FATAL_ERROR "Explicit timeout was overwritten: ${explicit_timeout}")
endif()
