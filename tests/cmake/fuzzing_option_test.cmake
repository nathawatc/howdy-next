cmake_minimum_required(VERSION 3.31)

set(howdy_source_dir "${CMAKE_CURRENT_LIST_DIR}/../..")
include("${howdy_source_dir}/cmake/howdy_fuzzing.cmake")

if(DEFINED ENV{HOWDY_FUZZING_TEST_CASE})
  set(test_build_testing ON)
  if(DEFINED ENV{HOWDY_FUZZING_BUILD_TESTING})
    set(test_build_testing "$ENV{HOWDY_FUZZING_BUILD_TESTING}")
  endif()
  howdy_parse_fuzzing_mode(
   "$ENV{HOWDY_FUZZING_TEST_CASE}"
   "${test_build_testing}"
   parsed_mode
  )
  message(STATUS "PARSED_MODE=${parsed_mode}")
  return()
endif()

if(NOT HOWDY_FUZZER_SANITIZER_FLAGS STREQUAL "-fsanitize=fuzzer,address,undefined")
  message(
   FATAL_ERROR
   "Unexpected HOWDY_FUZZER_SANITIZER_FLAGS: '${HOWDY_FUZZER_SANITIZER_FLAGS}'"
  )
endif()

set(valid_inputs AUTO auto Auto ON on On OFF off Off)
set(valid_expected AUTO AUTO AUTO ON ON ON OFF OFF OFF)
list(LENGTH valid_inputs valid_count)
math(EXPR valid_last_index "${valid_count} - 1")
foreach(index RANGE 0 ${valid_last_index})
  list(GET valid_inputs ${index} raw_value)
  list(GET valid_expected ${index} expected_mode)
  howdy_parse_fuzzing_mode("${raw_value}" ON parsed_mode)
  if(NOT parsed_mode STREQUAL "${expected_mode}")
    message(
     FATAL_ERROR
     "howdy_parse_fuzzing_mode('${raw_value}', ON) returned '${parsed_mode}', "
     "expected '${expected_mode}'"
    )
  endif()
endforeach()

set(no_test_valid_inputs AUTO auto OFF off)
set(no_test_valid_expected AUTO AUTO OFF OFF)
list(LENGTH no_test_valid_inputs no_test_count)
math(EXPR no_test_last_index "${no_test_count} - 1")
foreach(index RANGE 0 ${no_test_last_index})
  list(GET no_test_valid_inputs ${index} raw_value)
  list(GET no_test_valid_expected ${index} expected_mode)
  howdy_parse_fuzzing_mode("${raw_value}" OFF parsed_mode)
  if(NOT parsed_mode STREQUAL "${expected_mode}")
    message(
     FATAL_ERROR
     "howdy_parse_fuzzing_mode('${raw_value}', OFF) returned '${parsed_mode}', "
     "expected '${expected_mode}'"
    )
  endif()
endforeach()

function(expect_rejected raw_value build_testing expected_message)
  execute_process(
   COMMAND
    "${CMAKE_COMMAND}"
    -E env
    "HOWDY_FUZZING_TEST_CASE=${raw_value}"
    "HOWDY_FUZZING_BUILD_TESTING=${build_testing}"
    "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_LIST_FILE}"
   RESULT_VARIABLE parse_result
   OUTPUT_VARIABLE parse_output
   ERROR_VARIABLE parse_error
  )
  if(parse_result EQUAL 0)
    message(
     FATAL_ERROR
     "HOWDY_ENABLE_FUZZING='${raw_value}' (BUILD_TESTING=${build_testing}) "
     "was unexpectedly accepted:\n${parse_output}\n${parse_error}"
    )
  endif()
  string(FIND "${parse_output}\n${parse_error}" "${expected_message}" message_pos)
  if(message_pos EQUAL -1)
    message(
     FATAL_ERROR
     "Unexpected diagnostic for HOWDY_ENABLE_FUZZING='${raw_value}' "
     "(BUILD_TESTING=${build_testing}):\n${parse_output}\n${parse_error}"
    )
  endif()
endfunction()

foreach(on_value IN ITEMS ON on On)
  expect_rejected(
   "${on_value}"
   OFF
   "HOWDY_ENABLE_FUZZING=ON requires BUILD_TESTING=ON"
  )
endforeach()

set(
 invalid_inputs
 ""
 "FALSE"
 "false"
 "0"
 "NO"
 "TRUE"
 "1"
 "YES"
 "ONN"
 "OFFF"
 "AUTO "
 " OFF"
 "ON\;OFF"
 "bogus"
)
foreach(bad_value IN LISTS invalid_inputs)
  set(
   expected_error
   "Invalid HOWDY_ENABLE_FUZZING value '${bad_value}': expected AUTO, ON, or OFF"
  )
  expect_rejected("${bad_value}" ON "${expected_error}")
  expect_rejected("${bad_value}" OFF "${expected_error}")
endforeach()
