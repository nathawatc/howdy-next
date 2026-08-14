cmake_minimum_required(VERSION 3.31)

foreach(required_variable IN ITEMS
	HOWDY_VERSION_GENERATOR
	HOWDY_VERSION_TEMPLATE
	HOWDY_PROJECT_VERSION
	HOWDY_SOURCE_DIR
	HOWDY_TEST_ROOT
)
	if(
		NOT DEFINED ${required_variable}
		OR "${${required_variable}}" STREQUAL ""
	)
		message(
			FATAL_ERROR
			"Missing version-metadata test variable: "
			"${required_variable}"
		)
	endif()
endforeach()

if(NOT DEFINED HOWDY_GIT_EXECUTABLE)
	set(HOWDY_GIT_EXECUTABLE "")
endif()

file(REMOVE_RECURSE "${HOWDY_TEST_ROOT}")
file(MAKE_DIRECTORY "${HOWDY_TEST_ROOT}")
set(no_git_source "${HOWDY_TEST_ROOT}/no-git-source")
file(MAKE_DIRECTORY "${no_git_source}")

function(
	run_generator
	output_path
	source_path
	git_executable
	commit_override
	expect_success
)
	if(ARGC GREATER 5)
		set(expected_diagnostic "${ARGV5}")
	else()
		set(expected_diagnostic "HOWDY_GIT_COMMIT")
	endif()
	execute_process(
		COMMAND
			"${CMAKE_COMMAND}"
			"-DHOWDY_VERSION_TEMPLATE=${HOWDY_VERSION_TEMPLATE}"
			"-DHOWDY_VERSION_OUTPUT=${output_path}"
			"-DHOWDY_PROJECT_VERSION=${HOWDY_PROJECT_VERSION}"
			"-DHOWDY_SOURCE_DIR=${source_path}"
			"-DHOWDY_GIT_EXECUTABLE=${git_executable}"
			"-DHOWDY_GIT_COMMIT=${commit_override}"
			-P "${HOWDY_VERSION_GENERATOR}"
		RESULT_VARIABLE generator_result
		OUTPUT_VARIABLE generator_output
		ERROR_VARIABLE generator_error
	)
	if(expect_success)
		if(NOT "${generator_result}" STREQUAL "0")
			message(
				FATAL_ERROR
				"Version generator failed unexpectedly:\n"
				"${generator_output}\n"
				"${generator_error}"
			)
		endif()
	else()
		if("${generator_result}" STREQUAL "0")
			message(
				FATAL_ERROR
				"Malformed HOWDY_GIT_COMMIT unexpectedly "
				"succeeded"
			)
		endif()
		set(
			generator_diagnostic
			"${generator_output}\n${generator_error}"
		)
		string(
			FIND
			"${generator_diagnostic}"
			"${expected_diagnostic}"
			diagnostic_position
		)
		if(diagnostic_position EQUAL -1)
			message(
				FATAL_ERROR
				"Malformed metadata did not produce a clear "
				"diagnostic:\n"
				"${generator_diagnostic}"
			)
		endif()
	endif()
endfunction()

function(assert_generated_commit output_path expected_commit)
	if(NOT EXISTS "${output_path}")
		message(
			FATAL_ERROR
			"Version generator did not create ${output_path}"
		)
	endif()
	file(READ "${output_path}" generated_header)
	set(
		expected_line
		"kBuildCommit    = \"${expected_commit}\";"
	)
	string(FIND "${generated_header}" "${expected_line}" commit_position)
	if(commit_position EQUAL -1)
		message(
			FATAL_ERROR
			"Generated header has unexpected commit "
			"metadata; expected '${expected_commit}'"
		)
	endif()
endfunction()

set(full_override "0123456789abcdef0123456789abcdef01234567")
set(full_output "${HOWDY_TEST_ROOT}/full-override.hpp")
run_generator(
	"${full_output}"
	"${no_git_source}"
	""
	"${full_override}"
	TRUE
)
assert_generated_commit("${full_output}" "0123456789")
file(READ "${full_output}" full_header)
string(FIND "${full_header}" "${full_override}" full_sha_position)
if(full_sha_position GREATER -1)
	message(
		FATAL_ERROR
		"Generated header contains full HOWDY_GIT_COMMIT override"
	)
endif()

set(short_output "${HOWDY_TEST_ROOT}/short-override.hpp")
run_generator(
	"${short_output}"
	"${no_git_source}"
	""
	"abcdef1234"
	TRUE
)
assert_generated_commit("${short_output}" "abcdef1234")

set(uppercase_output "${HOWDY_TEST_ROOT}/uppercase-override.hpp")
run_generator(
	"${uppercase_output}"
	"${no_git_source}"
	""
	"ABCDEF1234567890"
	TRUE
)
assert_generated_commit("${uppercase_output}" "abcdef1234")

run_generator(
	"${HOWDY_TEST_ROOT}/short-malformed.hpp"
	"${no_git_source}"
	""
	"abc"
	FALSE
)
run_generator(
	"${HOWDY_TEST_ROOT}/non-hex-malformed.hpp"
	"${no_git_source}"
	""
	"01234567zz"
	FALSE
)

set(fake_git "${HOWDY_TEST_ROOT}/fake-git")
file(WRITE "${fake_git}" "#!/bin/sh\nprintf '%s\\n' abcdef1234\n")
file(CHMOD "${fake_git}" PERMISSIONS OWNER_READ OWNER_EXECUTE)
run_generator(
	"${HOWDY_TEST_ROOT}/git-ten.hpp"
	"${no_git_source}"
	"${fake_git}"
	""
	TRUE
)
assert_generated_commit("${HOWDY_TEST_ROOT}/git-ten.hpp" "abcdef1234")

file(WRITE "${fake_git}" "#!/bin/sh\nprintf '%s\\n' abcdef12345\n")
run_generator(
	"${HOWDY_TEST_ROOT}/git-eleven.hpp"
	"${no_git_source}"
	"${fake_git}"
	""
	TRUE
)
assert_generated_commit("${HOWDY_TEST_ROOT}/git-eleven.hpp" "abcdef12345")

file(WRITE "${fake_git}" "#!/bin/sh\nprintf '%s\\n' abcdef123456\n")
run_generator(
	"${HOWDY_TEST_ROOT}/git-twelve.hpp"
	"${no_git_source}"
	"${fake_git}"
	""
	TRUE
)
assert_generated_commit("${HOWDY_TEST_ROOT}/git-twelve.hpp" "abcdef123456")

file(WRITE "${fake_git}" "#!/bin/sh\nprintf '%s\\n' abcdef123\n")
run_generator(
	"${HOWDY_TEST_ROOT}/short-git.hpp"
	"${no_git_source}"
	"${fake_git}"
	""
	FALSE
	"Git returned malformed commit metadata"
)

file(WRITE "${fake_git}" "#!/bin/sh\nprintf '%s\\n' abcdef12zz\n")
run_generator(
	"${HOWDY_TEST_ROOT}/non-hex-git.hpp"
	"${no_git_source}"
	"${fake_git}"
	""
	FALSE
	"Git returned malformed commit metadata"
)

file(WRITE "${fake_git}" "#!/bin/sh\nprintf '\\n'")
run_generator(
	"${HOWDY_TEST_ROOT}/empty-git.hpp"
	"${no_git_source}"
	"${fake_git}"
	""
	FALSE
	"Git returned malformed commit metadata"
)

file(WRITE "${fake_git}" "#!/bin/sh\nprintf '%s\\n' malformed\n")
run_generator(
	"${HOWDY_TEST_ROOT}/malformed-git.hpp"
	"${no_git_source}"
	"${fake_git}"
	""
	FALSE
	"Git returned malformed commit metadata"
)

set(no_git_output "${HOWDY_TEST_ROOT}/no-git.hpp")
run_generator("${no_git_output}" "${no_git_source}" "" "" TRUE)
assert_generated_commit("${no_git_output}" "")

if(NOT "${HOWDY_GIT_EXECUTABLE}" STREQUAL "")
	set(git_source "${HOWDY_TEST_ROOT}/git-source")
	file(MAKE_DIRECTORY "${git_source}")

	function(run_git)
		execute_process(
			COMMAND "${HOWDY_GIT_EXECUTABLE}" ${ARGV}
			WORKING_DIRECTORY "${git_source}"
			RESULT_VARIABLE git_result
			OUTPUT_QUIET
			ERROR_VARIABLE git_error
		)
		if(NOT "${git_result}" STREQUAL "0")
			message(
				FATAL_ERROR
				"Git test command failed: ${git_error}"
			)
		endif()
	endfunction()

	run_git(init)
	run_git(config user.email howdy-version-test@example.invalid)
	run_git(config user.name Howdy-Version-Test)
	file(WRITE "${git_source}/marker.txt" "A\n")
	run_git(add marker.txt)
	run_git(commit --quiet -m commit-a)

	set(git_output "${HOWDY_TEST_ROOT}/git.hpp")
	run_generator(
		"${git_output}"
		"${git_source}"
		"${HOWDY_GIT_EXECUTABLE}"
		""
		TRUE
	)
	execute_process(
		COMMAND "${HOWDY_GIT_EXECUTABLE}" rev-parse --short=10 HEAD
		WORKING_DIRECTORY "${git_source}"
		RESULT_VARIABLE commit_a_result
		OUTPUT_VARIABLE commit_a
		OUTPUT_STRIP_TRAILING_WHITESPACE
	)
	if(NOT "${commit_a_result}" STREQUAL "0")
		message(
			FATAL_ERROR
			"Could not read first test commit"
		)
	endif()
	assert_generated_commit("${git_output}" "${commit_a}")
	file(READ "${git_output}" git_header_a)
	file(TIMESTAMP "${git_output}" timestamp_before)
	execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
	run_generator(
		"${git_output}"
		"${git_source}"
		"${HOWDY_GIT_EXECUTABLE}"
		""
		TRUE
	)
	file(TIMESTAMP "${git_output}" timestamp_after)
	if(NOT "${timestamp_before}" STREQUAL "${timestamp_after}")
		message(
			FATAL_ERROR
			"Unchanged version metadata rewrote generated header"
		)
	endif()

	file(WRITE "${git_source}/marker.txt" "B\n")
	run_git(add marker.txt)
	run_git(commit --quiet -m commit-b)
	run_generator(
		"${git_output}"
		"${git_source}"
		"${HOWDY_GIT_EXECUTABLE}"
		""
		TRUE
	)
	execute_process(
		COMMAND "${HOWDY_GIT_EXECUTABLE}" rev-parse --short=10 HEAD
		WORKING_DIRECTORY "${git_source}"
		RESULT_VARIABLE commit_b_result
		OUTPUT_VARIABLE commit_b
		OUTPUT_STRIP_TRAILING_WHITESPACE
	)
	if(NOT "${commit_b_result}" STREQUAL "0")
		message(
			FATAL_ERROR
			"Could not read second test commit"
		)
	endif()
	if("${commit_a}" STREQUAL "${commit_b}")
		message(
			FATAL_ERROR
			"Temporary Git test commits unexpectedly match"
		)
	endif()
	assert_generated_commit("${git_output}" "${commit_b}")
	file(READ "${git_output}" git_header_b)
	if("${git_header_a}" STREQUAL "${git_header_b}")
		message(
			FATAL_ERROR
			"Changed Git HEAD did not change generated metadata"
		)
	endif()
endif()

file(REMOVE_RECURSE "${HOWDY_TEST_ROOT}")
