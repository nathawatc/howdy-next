cmake_minimum_required(VERSION 3.31)

foreach(required_variable IN ITEMS
	HOWDY_SOURCE_DIR
	HOWDY_TEST_ROOT
	HOWDY_GIT_EXECUTABLE
)
	if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
		message(FATAL_ERROR
          "Missing tidy-incremental test variable: ${required_variable}")
	endif()
endforeach()

file(REMOVE_RECURSE "${HOWDY_TEST_ROOT}")
set(fixture_root "${HOWDY_TEST_ROOT}/fixture")
set(fake_bin "${fixture_root}/fake-bin")
set(build_dir "${fixture_root}/build/debug")
file(MAKE_DIRECTORY
	"${fixture_root}/cmake"
	"${fixture_root}/howdy/include"
	"${fixture_root}/howdy/src"
	"${fixture_root}/pam/src"
	"${fixture_root}/howdy/generated"
	"${fixture_root}/other"
	"${fake_bin}"
	"${build_dir}"
)
file(COPY "${HOWDY_SOURCE_DIR}/cmake/tidy_incremental.cmake"
     DESTINATION "${fixture_root}/cmake")
file(WRITE "${build_dir}/compile_commands.json" "[]\n")

function(run_git)
	execute_process(
		COMMAND "${HOWDY_GIT_EXECUTABLE}" ${ARGV}
		WORKING_DIRECTORY "${fixture_root}"
		RESULT_VARIABLE git_result
		OUTPUT_QUIET
		ERROR_VARIABLE git_error
	)
	if(NOT git_result EQUAL 0)
		message(FATAL_ERROR "Git fixture command failed: ${git_error}")
	endif()
endfunction()

file(WRITE "${fixture_root}/.gitignore" "howdy/generated/\n")
file(WRITE "${fixture_root}/howdy/src/tracked.cpp"
     "int tracked() { return 1; }\n")
file(WRITE "${fixture_root}/howdy/src/consumer.cpp"
     "int consumer() { return 1; }\n")
file(WRITE "${fixture_root}/howdy/src/ignored_consumer.cpp"
     "int ignored_consumer() { return 1; }\n")
run_git(init --quiet)
run_git(config user.email howdy-tidy-test@example.invalid)
run_git(config user.name Howdy-Tidy-Test)
run_git(
	add .gitignore howdy/src/tracked.cpp howdy/src/consumer.cpp
	howdy/src/ignored_consumer.cpp
)
run_git(-c commit.gpgsign=false commit --quiet -m baseline)

file(WRITE "${fixture_root}/howdy/src/tracked.cpp"
     "int tracked() { return 2; }\n")
file(WRITE "${fixture_root}/pam/src/new file.cpp" "int added() { return 1; }\n")
file(WRITE "${fixture_root}/howdy/include/new header.hpp" "#pragma once\n")
file(WRITE "${fixture_root}/howdy/generated/ignored.hpp" "#pragma once\n")
file(WRITE "${fixture_root}/pam/src/café.cpp"
     "int unicode_name() { return 1; }\n")
file(WRITE "${fixture_root}/pam/src/semi;colon.cpp"
     "int semicolon_name() { return 1; }\n")
file(WRITE "${fixture_root}/other/unrelated.cpp"
     "int unrelated() { return 1; }\n")

set(tracked_source "${fixture_root}/howdy/src/tracked.cpp")
set(new_source "${fixture_root}/pam/src/new file.cpp")
set(unicode_source "${fixture_root}/pam/src/café.cpp")
set(semicolon_source "${fixture_root}/pam/src/semi;colon.cpp")
set(consumer_source "${fixture_root}/howdy/src/consumer.cpp")
set(ignored_consumer_source "${fixture_root}/howdy/src/ignored_consumer.cpp")
set(unrelated_source "${fixture_root}/other/unrelated.cpp")
set(new_header "${fixture_root}/howdy/include/new header.hpp")
set(ignored_header "${fixture_root}/howdy/generated/ignored.hpp")
string(REPLACE " " "\\ " new_source_make "${new_source}")
string(REPLACE " " "\\ " new_header_make "${new_header}")
set(scan_deps "${fake_bin}/clang-scan-deps")
file(WRITE "${scan_deps}" "#!/bin/sh\ncat <<'EOF'\n")
file(APPEND "${scan_deps}" "tracked.o: ${tracked_source}\n")
file(APPEND "${scan_deps}" "new.o: ${new_source_make}\n")
file(APPEND "${scan_deps}" "unicode.o: ${unicode_source}\n")
file(APPEND "${scan_deps}" "semicolon.o: ${semicolon_source}\n")
file(APPEND "${scan_deps}"
     "consumer.o: ${consumer_source} ${new_header_make}\n")
file(APPEND "${scan_deps}"
     "ignored.o: ${ignored_consumer_source} ${ignored_header}\n")
file(APPEND "${scan_deps}" "unrelated.o: ${unrelated_source}\nEOF\n")
file(CHMOD "${scan_deps}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)

set(tidy_log "${fixture_root}/tidy-args.log")
set(run_tidy "${fake_bin}/run-clang-tidy")
file(WRITE "${run_tidy}"
     "#!/bin/sh\nprintf '%s\\n' \"$@\" > '${tidy_log}'\n")
file(CHMOD "${run_tidy}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)

set(old_path "$ENV{PATH}")
set(ENV{PATH} "${fake_bin}:${old_path}")
execute_process(
	COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${build_dir}" -DBASE=HEAD
		-P "${fixture_root}/cmake/tidy_incremental.cmake"
	WORKING_DIRECTORY "${fixture_root}"
	RESULT_VARIABLE tidy_result
	OUTPUT_VARIABLE tidy_output
	ERROR_VARIABLE tidy_error
)

if(NOT tidy_result EQUAL 0)
	message(FATAL_ERROR
         "Incremental tidy fixture failed:\n${tidy_output}\n${tidy_error}")
endif()
if(NOT EXISTS "${tidy_log}")
	message(FATAL_ERROR "run-clang-tidy fixture was not invoked")
endif()
file(READ "${tidy_log}" tidy_args)

function(assert_tidy_contains needle)
	string(FIND "${tidy_args}" "${needle}" position)
	if(position EQUAL -1)
		message(FATAL_ERROR "Missing tidy input '${needle}':\n${tidy_args}")
	endif()
endfunction()
function(assert_tidy_excludes needle)
	string(FIND "${tidy_args}" "${needle}" position)
	if(NOT position EQUAL -1)
		message(FATAL_ERROR "Unexpected tidy input '${needle}':\n${tidy_args}")
	endif()
endfunction()

assert_tidy_contains("howdy/src/tracked[.]cpp$")
assert_tidy_contains("pam/src/new file[.]cpp$")
assert_tidy_contains("pam/src/café[.]cpp$")
assert_tidy_contains("pam/src/semi;colon[.]cpp$")
assert_tidy_contains("howdy/src/consumer[.]cpp$")
assert_tidy_excludes("howdy/src/ignored_consumer[.]cpp$")
assert_tidy_excludes("other/unrelated[.]cpp$")

run_git(checkout -- howdy/src/tracked.cpp)
file(REMOVE "${tidy_log}")
execute_process(
	COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${build_dir}" -DBASE=HEAD
		-P "${fixture_root}/cmake/tidy_incremental.cmake"
	WORKING_DIRECTORY "${fixture_root}"
	RESULT_VARIABLE untracked_only_result
	OUTPUT_VARIABLE untracked_only_output
	ERROR_VARIABLE untracked_only_error
)
if(NOT untracked_only_result EQUAL 0)
	message(
		FATAL_ERROR
		"Untracked-only tidy fixture failed:\n"
		"${untracked_only_output}\n${untracked_only_error}"
	)
endif()
file(READ "${tidy_log}" tidy_args)
assert_tidy_contains("pam/src/new file[.]cpp$")
assert_tidy_contains("pam/src/café[.]cpp$")
assert_tidy_contains("pam/src/semi;colon[.]cpp$")
assert_tidy_contains("howdy/src/consumer[.]cpp$")
assert_tidy_excludes("howdy/src/tracked[.]cpp$")
assert_tidy_excludes("howdy/src/ignored_consumer[.]cpp$")
assert_tidy_excludes("other/unrelated[.]cpp$")

file(WRITE "${run_tidy}" "#!/bin/sh\nexit 17\n")
file(CHMOD "${run_tidy}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
execute_process(
	COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${build_dir}" -DBASE=HEAD
		-P "${fixture_root}/cmake/tidy_incremental.cmake"
	WORKING_DIRECTORY "${fixture_root}"
	RESULT_VARIABLE failure_result
	OUTPUT_VARIABLE failure_output
	ERROR_VARIABLE failure_error
)
set(ENV{PATH} "${old_path}")
if(failure_result EQUAL 0)
	message(
		FATAL_ERROR
		"run-clang-tidy failure was not propagated:\n"
		"${failure_output}\n${failure_error}"
	)
endif()

file(REMOVE_RECURSE "${HOWDY_TEST_ROOT}")
