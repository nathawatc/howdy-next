cmake_minimum_required(VERSION 3.31)

foreach(required_variable IN ITEMS HOWDY_SOURCE_DIR HOWDY_TEST_ROOT HOWDY_CMAKE_GENERATOR)
	if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
		message(FATAL_ERROR "Missing timeout test variable: ${required_variable}")
	endif()
endforeach()

file(REMOVE_RECURSE "${HOWDY_TEST_ROOT}")
execute_process(
	COMMAND
		"${CMAKE_COMMAND}"
		-S "${HOWDY_SOURCE_DIR}/tests/cmake/timeout_defaults_fixture"
		-B "${HOWDY_TEST_ROOT}"
		-G "${HOWDY_CMAKE_GENERATOR}"
		"-DHOWDY_SOURCE_DIR=${HOWDY_SOURCE_DIR}"
	RESULT_VARIABLE configure_result
	OUTPUT_VARIABLE configure_output
	ERROR_VARIABLE configure_error
)
if(NOT configure_result EQUAL 0)
	message(
		FATAL_ERROR
		"Timeout fixture configure failed:\n${configure_output}\n${configure_error}"
	)
endif()
file(REMOVE_RECURSE "${HOWDY_TEST_ROOT}")
