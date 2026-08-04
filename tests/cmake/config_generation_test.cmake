cmake_minimum_required(VERSION 3.31)

foreach(required_variable IN ITEMS
	HOWDY_SOURCE_DIR
	HOWDY_TEST_ROOT
	HOWDY_CMAKE_GENERATOR
	HOWDY_PAM_INCLUDE_DIR
	HOWDY_PAM_LIBRARY
)
	if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
		message(
			FATAL_ERROR
			"Missing config-generation test variable: ${required_variable}"
		)
	endif()
endforeach()

find_program(HOWDY_TRUE_EXECUTABLE true REQUIRED)
find_program(HOWDY_ENV_EXECUTABLE env REQUIRED)
file(REMOVE_RECURSE "${HOWDY_TEST_ROOT}")
file(MAKE_DIRECTORY "${HOWDY_TEST_ROOT}")

function(configure_case case_name emulator_value)
	set(case_dir "${HOWDY_TEST_ROOT}/${case_name}")
	set(configure_arguments
		-S "${HOWDY_SOURCE_DIR}"
		-B "${case_dir}"
		-G "${HOWDY_CMAKE_GENERATOR}"
		-DCMAKE_SYSTEM_NAME=Linux
		-DCMAKE_BUILD_TYPE=Debug
		-DBUILD_TESTING=OFF
		"-DPAM_INCLUDE_DIR=${HOWDY_PAM_INCLUDE_DIR}"
		"-DPAM_LIBRARY=${HOWDY_PAM_LIBRARY}"
	)
	if(NOT emulator_value STREQUAL "__UNSET__")
		list(APPEND configure_arguments
			"-DCMAKE_CROSSCOMPILING_EMULATOR=${emulator_value}"
		)
	endif()

	execute_process(
		COMMAND "${CMAKE_COMMAND}" ${configure_arguments}
		RESULT_VARIABLE configure_result
		OUTPUT_VARIABLE configure_output
		ERROR_VARIABLE configure_error
	)
	set(CONFIGURE_RESULT "${configure_result}" PARENT_SCOPE)
	set(CONFIGURE_OUTPUT "${configure_output}\n${configure_error}" PARENT_SCOPE)
	set(CONFIGURE_CASE_DIR "${case_dir}" PARENT_SCOPE)
endfunction()

configure_case(no-emulator __UNSET__)
if(CONFIGURE_RESULT EQUAL 0)
	message(FATAL_ERROR "Cross configure without emulator unexpectedly succeeded")
endif()
string(
	FIND
	"${CONFIGURE_OUTPUT}"
	"Cross-compiling Howdy requires CMAKE_CROSSCOMPILING_EMULATOR"
	no_emulator_message
)
if(no_emulator_message EQUAL -1)
	message(
		FATAL_ERROR
		"Cross configure without emulator omitted its diagnostic:\n"
		"${CONFIGURE_OUTPUT}"
	)
endif()

configure_case(off-emulator OFF)
if(CONFIGURE_RESULT EQUAL 0)
	message(FATAL_ERROR "Cross configure with OFF emulator unexpectedly succeeded")
endif()
string(
	FIND
	"${CONFIGURE_OUTPUT}"
	"Cross-compiling Howdy requires CMAKE_CROSSCOMPILING_EMULATOR"
	off_emulator_message
)
if(off_emulator_message EQUAL -1)
	message(
		FATAL_ERROR
		"Cross configure with OFF emulator omitted its diagnostic:\n"
		"${CONFIGURE_OUTPUT}"
	)
endif()

configure_case(no-execution "${HOWDY_TRUE_EXECUTABLE}")
if(NOT CONFIGURE_RESULT EQUAL 0)
	message(
		FATAL_ERROR
		"No-execution emulator configure failed:\n${CONFIGURE_OUTPUT}"
	)
endif()
execute_process(
	COMMAND
		"${CMAKE_COMMAND}"
		--build "${CONFIGURE_CASE_DIR}"
		--target howdy_packaged_config
		--parallel 2
	RESULT_VARIABLE no_execution_build_result
	OUTPUT_VARIABLE no_execution_build_output
	ERROR_VARIABLE no_execution_build_error
)
if(no_execution_build_result EQUAL 0)
	message(
		FATAL_ERROR
		"Emulator that skips generator unexpectedly produced a config"
	)
endif()
set(
	no_execution_output
	"${no_execution_build_output}\n${no_execution_build_error}"
)
string(
	FIND
	"${no_execution_output}"
	"did not produce a regular candidate file"
	no_execution_message
)
if(no_execution_message EQUAL -1)
	message(
		FATAL_ERROR
		"Missing-candidate build omitted its diagnostic:\n${no_execution_output}"
	)
endif()
if(EXISTS "${CONFIGURE_CASE_DIR}/generated/config/config.ini")
	message(FATAL_ERROR "Missing-candidate build left a final generated config")
endif()

configure_case(working-emulator "${HOWDY_ENV_EXECUTABLE}")
if(NOT CONFIGURE_RESULT EQUAL 0)
	message(FATAL_ERROR "Working emulator configure failed:\n${CONFIGURE_OUTPUT}")
endif()
execute_process(
	COMMAND
		"${CMAKE_COMMAND}"
		--build "${CONFIGURE_CASE_DIR}"
		--target howdy_packaged_config
		--parallel 2
	RESULT_VARIABLE working_build_result
	OUTPUT_VARIABLE working_build_output
	ERROR_VARIABLE working_build_error
)
if(NOT working_build_result EQUAL 0)
	message(
		FATAL_ERROR
		"Working emulator failed to generate packaged config:\n"
		"${working_build_output}\n${working_build_error}"
	)
endif()
set(working_output "${CONFIGURE_CASE_DIR}/generated/config/config.ini")
if(NOT EXISTS "${working_output}" OR IS_DIRECTORY "${working_output}")
	message(FATAL_ERROR "Working emulator did not produce ${working_output}")
endif()
file(SIZE "${working_output}" working_size)
if(working_size LESS 1)
	message(FATAL_ERROR "Working emulator produced empty ${working_output}")
endif()

file(REMOVE_RECURSE "${HOWDY_TEST_ROOT}")
