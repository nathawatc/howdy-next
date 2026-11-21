cmake_minimum_required(VERSION 3.31)

foreach(required_variable IN ITEMS
 HOWDY_VERSION_TEMPLATE
 HOWDY_VERSION_OUTPUT
 HOWDY_PROJECT_VERSION
 HOWDY_SOURCE_DIR
)
  if(
  NOT DEFINED ${required_variable}
  OR "${${required_variable}}" STREQUAL ""
 )
    message(
   FATAL_ERROR
   "Missing version-generation variable: "
   "${required_variable}"
  )
  endif()
endforeach()

if(NOT DEFINED HOWDY_GIT_EXECUTABLE)
  set(HOWDY_GIT_EXECUTABLE "")
endif()
if(NOT DEFINED HOWDY_GIT_COMMIT)
  set(HOWDY_GIT_COMMIT "")
endif()

function(
 normalize_commit
 raw_commit
 error_prefix
 hex_qualifier
 truncate_to_10
 output_variable
)
  string(LENGTH "${raw_commit}" raw_commit_length)
  if(raw_commit_length LESS 10)
    message(
   FATAL_ERROR
   "${error_prefix} at least 10 hexadecimal characters"
  )
  endif()
  string(
  REGEX MATCH
  "^[0-9A-Fa-f]+$"
  raw_commit_hex
  "${raw_commit}"
 )
  if("${raw_commit_hex}" STREQUAL "")
    message(
   FATAL_ERROR
   "${error_prefix}${hex_qualifier} hexadecimal characters"
  )
  endif()
  if(truncate_to_10)
    string(SUBSTRING "${raw_commit}" 0 10 normalized_commit)
  else()
    set(normalized_commit "${raw_commit}")
  endif()
  string(TOLOWER "${normalized_commit}" normalized_commit)
  set("${output_variable}" "${normalized_commit}" PARENT_SCOPE)
endfunction()

set(HOWDY_BUILD_COMMIT "")
if(NOT "${HOWDY_GIT_COMMIT}" STREQUAL "")
  normalize_commit(
  "${HOWDY_GIT_COMMIT}"
  "HOWDY_GIT_COMMIT must contain"
  " only"
  TRUE
  HOWDY_BUILD_COMMIT
 )
elseif(NOT "${HOWDY_GIT_EXECUTABLE}" STREQUAL "")
  execute_process(
  COMMAND
   "${HOWDY_GIT_EXECUTABLE}"
   rev-parse --short=10 HEAD
  WORKING_DIRECTORY "${HOWDY_SOURCE_DIR}"
  RESULT_VARIABLE git_result
  OUTPUT_VARIABLE git_output
  ERROR_QUIET
  OUTPUT_STRIP_TRAILING_WHITESPACE
 )
  if("${git_result}" STREQUAL "0")
    normalize_commit(
   "${git_output}"
   "Git returned malformed commit metadata; expected"
   ""
   FALSE
   HOWDY_BUILD_COMMIT
  )
  endif()
endif()

set(PROJECT_VERSION "${HOWDY_PROJECT_VERSION}")
get_filename_component(
 version_output_directory
 "${HOWDY_VERSION_OUTPUT}"
 DIRECTORY
)
file(MAKE_DIRECTORY "${version_output_directory}")
set(version_candidate "${HOWDY_VERSION_OUTPUT}.tmp")
configure_file(
 "${HOWDY_VERSION_TEMPLATE}"
 "${version_candidate}"
 @ONLY
)
execute_process(
 COMMAND
  "${CMAKE_COMMAND}"
  -E copy_if_different
  "${version_candidate}"
  "${HOWDY_VERSION_OUTPUT}"
 RESULT_VARIABLE copy_result
)
file(REMOVE "${version_candidate}")
if(NOT "${copy_result}" STREQUAL "0")
  message(FATAL_ERROR "Could not publish generated version header")
endif()
