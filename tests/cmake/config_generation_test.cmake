cmake_minimum_required(VERSION 3.31)

if(DEFINED HOWDY_CONFIG_TEST_MODE)
	include("${HOWDY_SOURCE_DIR}/cmake/howdy_cross_compilation.cmake")
	if(HOWDY_CONFIG_TEST_MODE STREQUAL "policy")
		set(CMAKE_CROSSCOMPILING TRUE)
		if(HOWDY_TEST_EMULATOR STREQUAL "__UNSET__")
			unset(CMAKE_CROSSCOMPILING_EMULATOR)
		else()
			set(CMAKE_CROSSCOMPILING_EMULATOR "${HOWDY_TEST_EMULATOR}")
		endif()
		howdy_require_crosscompiling_emulator()
	else()
		message(
			FATAL_ERROR
			"Unknown config-generation test mode: ${HOWDY_CONFIG_TEST_MODE}"
		)
	endif()
	return()
endif()

foreach(required_variable IN ITEMS
	HOWDY_SOURCE_DIR
	HOWDY_TEST_ROOT
	HOWDY_CMAKE_GENERATOR
	HOWDY_CONFIG_GENERATOR
	HOWDY_PACKAGED_CONFIG_SCRIPT
)
	if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
		message(
			FATAL_ERROR
			"Missing config-generation test variable: ${required_variable}"
		)
	endif()
endforeach()

include("${HOWDY_SOURCE_DIR}/cmake/howdy_cross_compilation.cmake")
find_program(HOWDY_TRUE_EXECUTABLE true REQUIRED)
file(REMOVE_RECURSE "${HOWDY_TEST_ROOT}")
file(MAKE_DIRECTORY "${HOWDY_TEST_ROOT}")

function(run_policy_case emulator_value)
	execute_process(
		COMMAND
			"${CMAKE_COMMAND}"
			"-DHOWDY_SOURCE_DIR=${HOWDY_SOURCE_DIR}"
			-DHOWDY_CONFIG_TEST_MODE=policy
			"-DHOWDY_TEST_EMULATOR=${emulator_value}"
			-P "${CMAKE_CURRENT_LIST_FILE}"
		RESULT_VARIABLE policy_result
		OUTPUT_VARIABLE policy_output
		ERROR_VARIABLE policy_error
	)
	set(POLICY_RESULT "${policy_result}" PARENT_SCOPE)
	set(POLICY_OUTPUT "${policy_output}\n${policy_error}" PARENT_SCOPE)
endfunction()

set(expected_policy_message
	"Cross-compiling Howdy requires CMAKE_CROSSCOMPILING_EMULATOR"
)
run_policy_case(__UNSET__)
if(POLICY_RESULT EQUAL 0)
	message(FATAL_ERROR "Cross configure without emulator unexpectedly succeeded")
endif()
string(FIND "${POLICY_OUTPUT}" "${expected_policy_message}" no_emulator_message)
if(no_emulator_message EQUAL -1)
	message(
		FATAL_ERROR
		"Cross configure without emulator omitted its diagnostic:\n${POLICY_OUTPUT}"
	)
endif()

run_policy_case(OFF)
if(POLICY_RESULT EQUAL 0)
	message(FATAL_ERROR "Cross configure with OFF emulator unexpectedly succeeded")
endif()
string(FIND "${POLICY_OUTPUT}" "${expected_policy_message}"
       off_emulator_message)
if(off_emulator_message EQUAL -1)
	message(
		FATAL_ERROR
		"Cross configure with OFF emulator omitted its diagnostic:\n${POLICY_OUTPUT}"
	)
endif()

run_policy_case("${HOWDY_TRUE_EXECUTABLE}")
if(NOT POLICY_RESULT EQUAL 0)
	message(
		FATAL_ERROR
		"Cross configure with emulator failed:\n${POLICY_OUTPUT}"
	)
endif()

set(target_property_dir "${HOWDY_TEST_ROOT}/target-property")
execute_process(
	COMMAND
		"${CMAKE_COMMAND}"
		-S "${HOWDY_SOURCE_DIR}/tests/cmake/cross_compilation_fixture"
		-B "${target_property_dir}"
		-G "${HOWDY_CMAKE_GENERATOR}"
		-DCMAKE_SYSTEM_NAME=Linux
		"-DCMAKE_CROSSCOMPILING_EMULATOR=${HOWDY_TRUE_EXECUTABLE}"
		"-DHOWDY_SOURCE_DIR=${HOWDY_SOURCE_DIR}"
	RESULT_VARIABLE target_property_result
	OUTPUT_VARIABLE target_property_output
	ERROR_VARIABLE target_property_error
)
if(NOT target_property_result EQUAL 0)
	message(
		FATAL_ERROR
		"Cross-compiling target property fixture failed:\n"
		"${target_property_output}\n${target_property_error}"
	)
endif()

function(run_packaged_action action candidate output)
	execute_process(
		COMMAND
			"${CMAKE_COMMAND}"
			"-DHOWDY_PACKAGED_CONFIG_ACTION=${action}"
			"-DHOWDY_PACKAGED_CONFIG_CANDIDATE=${candidate}"
			"-DHOWDY_PACKAGED_CONFIG_OUTPUT=${output}"
			-P "${HOWDY_PACKAGED_CONFIG_SCRIPT}"
		RESULT_VARIABLE packaged_result
		OUTPUT_VARIABLE packaged_output
		ERROR_VARIABLE packaged_error
	)
	set(PACKAGED_RESULT "${packaged_result}" PARENT_SCOPE)
	set(PACKAGED_OUTPUT "${packaged_output}\n${packaged_error}" PARENT_SCOPE)
endfunction()

set(packaged_root "${HOWDY_TEST_ROOT}/packaged")
set(packaged_candidate "${packaged_root}/generated/config.ini.candidate")
set(packaged_output "${packaged_root}/generated/config.ini")
file(MAKE_DIRECTORY "${packaged_root}/generated")
run_packaged_action(prepare "${packaged_candidate}" "${packaged_output}")
if(NOT PACKAGED_RESULT EQUAL 0)
	message(FATAL_ERROR "Packaged-config prepare failed:\n${PACKAGED_OUTPUT}")
endif()

execute_process(
	COMMAND
		"${HOWDY_CONFIG_GENERATOR}"
		--output "${packaged_candidate}"
	RESULT_VARIABLE generator_result
	OUTPUT_VARIABLE generator_output
	ERROR_VARIABLE generator_error
)
if(NOT generator_result EQUAL 0)
	message(
		FATAL_ERROR
		"Built packaged-config generator failed:\n"
		"${generator_output}\n${generator_error}"
	)
endif()

run_packaged_action(publish "${packaged_candidate}" "${packaged_output}")
if(NOT PACKAGED_RESULT EQUAL 0)
	message(FATAL_ERROR "Packaged-config publish failed:\n${PACKAGED_OUTPUT}")
endif()
if(NOT EXISTS "${packaged_output}"
	OR IS_DIRECTORY "${packaged_output}"
	OR IS_SYMLINK "${packaged_output}"
)
	message(FATAL_ERROR "Packaged-config publish did not create regular output")
endif()
file(SIZE "${packaged_output}" packaged_size)
if(packaged_size LESS 1)
	message(FATAL_ERROR "Packaged-config publish created empty output")
endif()
file(READ "${packaged_output}" packaged_content)
string(FIND "${packaged_content}" "[core]" packaged_section)
if(packaged_section EQUAL -1)
	message(FATAL_ERROR "Packaged-config output omitted [core] section")
endif()

set(missing_root "${HOWDY_TEST_ROOT}/missing-candidate")
set(missing_candidate "${missing_root}/generated/config.ini.candidate")
set(missing_output "${missing_root}/generated/config.ini")
file(MAKE_DIRECTORY "${missing_root}/generated")
run_packaged_action(prepare "${missing_candidate}" "${missing_output}")
if(NOT PACKAGED_RESULT EQUAL 0)
	message(FATAL_ERROR "Missing-candidate prepare failed:\n${PACKAGED_OUTPUT}")
endif()
run_packaged_action(publish "${missing_candidate}" "${missing_output}")
if(PACKAGED_RESULT EQUAL 0)
	message(FATAL_ERROR "Missing candidate unexpectedly published")
endif()
string(
	FIND
	"${PACKAGED_OUTPUT}"
	"did not produce a regular candidate file"
	missing_candidate_message
)
if(missing_candidate_message EQUAL -1)
	message(
		FATAL_ERROR
		"Missing-candidate publish omitted its diagnostic:\n${PACKAGED_OUTPUT}"
	)
endif()
if(EXISTS "${missing_output}")
	message(FATAL_ERROR "Missing-candidate publish left a final generated config")
endif()

file(REMOVE_RECURSE "${HOWDY_TEST_ROOT}")
