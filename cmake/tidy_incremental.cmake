cmake_minimum_required(VERSION 3.31)

if(NOT DEFINED BUILD_DIR)
  message(FATAL_ERROR "BUILD_DIR is required")
endif()
if(NOT DEFINED BASE)
  set(BASE HEAD)
endif()

file(REAL_PATH "${CMAKE_CURRENT_LIST_DIR}/.." project_root)
cmake_path(
 ABSOLUTE_PATH BUILD_DIR
 BASE_DIRECTORY "${project_root}"
 NORMALIZE
 OUTPUT_VARIABLE build_dir
)
set(compile_commands "${build_dir}/compile_commands.json")
if(NOT EXISTS "${compile_commands}")
  message(FATAL_ERROR "Missing ${compile_commands}")
endif()

find_program(git_executable NAMES git REQUIRED)
find_program(clang_scan_deps_executable NAMES clang-scan-deps REQUIRED)
find_program(run_clang_tidy_executable NAMES run-clang-tidy REQUIRED)

execute_process(COMMAND "${git_executable}" -c core.quotePath=false diff
                --name-only "${BASE}" -- howdy pam WORKING_DIRECTORY
                "${project_root}" OUTPUT_VARIABLE changed_output
                COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${git_executable}" -c core.quotePath=false ls-files
                --others --exclude-standard -- howdy pam WORKING_DIRECTORY
                "${project_root}" OUTPUT_VARIABLE untracked_output
                COMMAND_ERROR_IS_FATAL ANY)
set(worktree_output "${changed_output}${untracked_output}")
string(REPLACE "\r\n" "\n" worktree_output "${worktree_output}")
string(REGEX REPLACE "\n+$" "" worktree_output "${worktree_output}")
if(worktree_output STREQUAL "")
  message(STATUS "No changed files under howdy/ or pam/")
  return()
endif()
string(REPLACE ";" "\\;" worktree_output "${worktree_output}")
string(REPLACE "\n" ";" changed_files "${worktree_output}")

set(changed_sources)
set(changed_headers)
foreach(path IN LISTS changed_files)
  get_filename_component(extension "${path}" EXT)
  string(TOLOWER "${extension}" extension)
  if(extension MATCHES "^\\.(c|cc|cpp|cxx)$")
    cmake_path(
   ABSOLUTE_PATH path
   BASE_DIRECTORY "${project_root}"
   NORMALIZE
   OUTPUT_VARIABLE absolute_path
  )
    string(REPLACE ";" "\\;" absolute_path "${absolute_path}")
    list(APPEND changed_sources "${absolute_path}")
  elseif(extension MATCHES "^\\.(h|hh|hpp|hxx|inc)$")
    cmake_path(
   ABSOLUTE_PATH path
   BASE_DIRECTORY "${project_root}"
   NORMALIZE
   OUTPUT_VARIABLE absolute_path
  )
    string(REPLACE ";" "\\;" absolute_path "${absolute_path}")
    list(APPEND changed_headers "${absolute_path}")
  endif()
endforeach()
if(NOT changed_sources AND NOT changed_headers)
  message(STATUS "No changed C/C++ sources or headers")
  return()
endif()

execute_process(
 COMMAND
  "${clang_scan_deps_executable}"
  "-compilation-database=${compile_commands}"
  -format=make
 WORKING_DIRECTORY "${project_root}"
 OUTPUT_VARIABLE dependency_output
 ERROR_VARIABLE dependency_error
 RESULT_VARIABLE dependency_result
)
if(NOT dependency_result EQUAL 0)
  message(FATAL_ERROR "clang-scan-deps failed:\n${dependency_error}")
endif()

string(REPLACE "\\\n" " " dependency_output "${dependency_output}")
string(REPLACE "\r\n" "\n" dependency_output "${dependency_output}")
string(REPLACE ";" "\\;" dependency_output "${dependency_output}")
string(REPLACE "\n" ";" dependency_rules "${dependency_output}")

set(affected_sources)
foreach(rule IN LISTS dependency_rules)
  if(NOT rule MATCHES ":")
    continue()
  endif()
  string(REGEX REPLACE "^[^:]*:[ ]*" "" raw_dependencies "${rule}")
  if(raw_dependencies STREQUAL "")
    continue()
  endif()
  separate_arguments(dependencies UNIX_COMMAND "${raw_dependencies}")
  list(GET dependencies 0 source)
  cmake_path(NORMAL_PATH source OUTPUT_VARIABLE source)
  if(NOT IS_ABSOLUTE "${source}")
    cmake_path(
   ABSOLUTE_PATH source
   BASE_DIRECTORY "${project_root}"
   NORMALIZE
   OUTPUT_VARIABLE source
  )
  endif()

  set(is_affected FALSE)
  list(FIND changed_sources "${source}" source_index)
  if(NOT source_index EQUAL -1)
    set(is_affected TRUE)
  endif()
  if(NOT is_affected)
    foreach(header IN LISTS changed_headers)
      list(FIND dependencies "${header}" header_index)
      if(NOT header_index EQUAL -1)
        set(is_affected TRUE)
        break()
      endif()
    endforeach()
  endif()
  if(is_affected)
    list(FIND affected_sources "${source}" affected_index)
    if(affected_index EQUAL -1)
      string(REPLACE ";" "\\;" source "${source}")
      list(APPEND affected_sources "${source}")
    endif()
  endif()
endforeach()
if(NOT affected_sources)
  message(STATUS "No compile-database translation units are affected")
  return()
endif()
set(tidy_patterns)
foreach(source IN LISTS affected_sources)
  file(RELATIVE_PATH relative_source "${project_root}" "${source}")
  string(REPLACE "." "[.]" source_pattern "${relative_source}")
  string(REPLACE ";" "\\;" source_pattern "${source_pattern}")
  list(APPEND tidy_patterns "${source_pattern}$")
endforeach()

list(LENGTH affected_sources affected_count)
message(STATUS
        "Running clang-tidy on ${affected_count} affected translation unit(s)")
foreach(source IN LISTS affected_sources)
  file(RELATIVE_PATH relative_source "${project_root}" "${source}")
  message(STATUS "  ${relative_source}")
endforeach()

execute_process(
 COMMAND
  "${run_clang_tidy_executable}"
  -p "${build_dir}"
  -quiet
  -hide-progress
  ${tidy_patterns}
 WORKING_DIRECTORY "${project_root}"
 COMMAND_ERROR_IS_FATAL ANY
)
