cmake_minimum_required(VERSION 3.31)

foreach(required_variable IN ITEMS HOWDY_SOURCE_DIR HOWDY_TEST_ROOT)
  if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
    message(FATAL_ERROR "Missing required variable: ${required_variable}")
  endif()
endforeach()

file(REMOVE_RECURSE "${HOWDY_TEST_ROOT}")
file(MAKE_DIRECTORY "${HOWDY_TEST_ROOT}")

set(test_emulator "/usr/bin/qemu-dummy")
set(block_regex "set\\(howdy_preflight_emulator\\)[\r\n\t ]*if\\([^)]*\\)[\r\n\t ]*set\\(howdy_preflight_emulator [^)]*\\)[\r\n\t ]*endif\\(\\)")

# Native mode: CMAKE_CROSSCOMPILING FALSE
set(CMAKE_CROSSCOMPILING FALSE)
set(CMAKE_CROSSCOMPILING_EMULATOR "${test_emulator}")
include("${HOWDY_SOURCE_DIR}/cmake/howdy_cross_compilation.cmake")

if(NOT "${howdy_test_emulator}" STREQUAL "")
  message(FATAL_ERROR "Native build populated howdy_test_emulator: ${howdy_test_emulator}")
endif()

set(PROJECT_BINARY_DIR "${HOWDY_TEST_ROOT}/native")
configure_file(
 "${HOWDY_SOURCE_DIR}/cmake/install_preflight.cmake.in"
 "${HOWDY_TEST_ROOT}/native_install_preflight.cmake"
 @ONLY
)

file(READ "${HOWDY_TEST_ROOT}/native_install_preflight.cmake" native_preflight)
string(REGEX MATCH "${block_regex}" native_block "${native_preflight}")
if(NOT native_block MATCHES "if\\(FALSE\\)")
  message(FATAL_ERROR "Native install preflight did not disable cross-compiling condition")
endif()

file(WRITE "${HOWDY_TEST_ROOT}/eval_native.cmake"
 "set(howdy_preflight_emulator \"sentinel\")\n${native_block}\nif(NOT \"\${howdy_preflight_emulator}\" STREQUAL \"\")\n  message(FATAL_ERROR \"Native preflight set emulator: \${howdy_preflight_emulator}\")\nendif()\n"
)
execute_process(
 COMMAND "${CMAKE_COMMAND}" -P "${HOWDY_TEST_ROOT}/eval_native.cmake"
 RESULT_VARIABLE native_eval_result
 OUTPUT_VARIABLE native_eval_output
 ERROR_VARIABLE native_eval_error
)
if(NOT native_eval_result EQUAL 0)
  message(FATAL_ERROR "Native preflight evaluation failed:\n${native_eval_output}\n${native_eval_error}")
endif()

# Cross mode: CMAKE_CROSSCOMPILING TRUE
set(CMAKE_CROSSCOMPILING TRUE)
set(CMAKE_CROSSCOMPILING_EMULATOR "${test_emulator}")
include("${HOWDY_SOURCE_DIR}/cmake/howdy_cross_compilation.cmake")

if(NOT "${howdy_test_emulator}" STREQUAL "${test_emulator}")
  message(FATAL_ERROR "Cross build failed to populate howdy_test_emulator: ${howdy_test_emulator}")
endif()

set(PROJECT_BINARY_DIR "${HOWDY_TEST_ROOT}/cross")
configure_file(
 "${HOWDY_SOURCE_DIR}/cmake/install_preflight.cmake.in"
 "${HOWDY_TEST_ROOT}/cross_install_preflight.cmake"
 @ONLY
)

file(READ "${HOWDY_TEST_ROOT}/cross_install_preflight.cmake" cross_preflight)
string(REGEX MATCH "${block_regex}" cross_block "${cross_preflight}")
if(NOT cross_block MATCHES "if\\(TRUE\\)")
  message(FATAL_ERROR "Cross install preflight did not enable cross-compiling condition")
endif()
if(NOT cross_block MATCHES "\\[==\\[${test_emulator}\\]==\\]")
  message(FATAL_ERROR "Cross install preflight missing configured emulator path")
endif()

file(WRITE "${HOWDY_TEST_ROOT}/eval_cross.cmake"
 "set(howdy_preflight_emulator)\n${cross_block}\nif(NOT \"\${howdy_preflight_emulator}\" STREQUAL \"${test_emulator}\")\n  message(FATAL_ERROR \"Cross preflight failed to set emulator: \${howdy_preflight_emulator}\")\nendif()\n"
)
execute_process(
 COMMAND "${CMAKE_COMMAND}" -P "${HOWDY_TEST_ROOT}/eval_cross.cmake"
 RESULT_VARIABLE cross_eval_result
 OUTPUT_VARIABLE cross_eval_output
 ERROR_VARIABLE cross_eval_error
)
if(NOT cross_eval_result EQUAL 0)
  message(FATAL_ERROR "Cross preflight evaluation failed:\n${cross_eval_output}\n${cross_eval_error}")
endif()

file(REMOVE_RECURSE "${HOWDY_TEST_ROOT}")
