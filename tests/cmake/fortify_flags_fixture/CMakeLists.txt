cmake_minimum_required(VERSION 3.31)

project(howdy_fortify_flags_fixture LANGUAGES NONE)

if(NOT DEFINED HOWDY_SOURCE_DIR OR "${HOWDY_SOURCE_DIR}" STREQUAL "")
  message(FATAL_ERROR "HOWDY_SOURCE_DIR is required")
endif()
include("${HOWDY_SOURCE_DIR}/cmake/howdy_fortify.cmake")

howdy_add_fortify_source()

get_directory_property(fortify_definitions COMPILE_DEFINITIONS)
file(WRITE "${CMAKE_BINARY_DIR}/definitions.txt" "${fortify_definitions}")
