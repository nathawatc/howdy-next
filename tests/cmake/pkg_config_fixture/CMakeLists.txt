cmake_minimum_required(VERSION 3.31)
project(howdy_pkg_config_fixture LANGUAGES NONE)

include("${HOWDY_SOURCE_DIR}/cmake/howdy_cross_compilation.cmake")
find_package(PkgConfig REQUIRED)
include("${HOWDY_SOURCE_DIR}/cmake/howdy_pkg_config.cmake")

set(query "howdy-test-main>=1.0")
if(DEFINED HOWDY_TEST_QUERY)
  set(query "${HOWDY_TEST_QUERY}")
endif()

if(HOWDY_TEST_PLAIN_LOOKUP)
  # Cache written by plain pkg_check_modules(), as before howdy_pkg_check_modules() existed
  pkg_check_modules(HOWDY_TEST_MAIN REQUIRED ${query})
  return()
endif()

if(HOWDY_SEED_HOST_CACHE)
  # Pre-seed cache with initial pkg-config lookup
  howdy_pkg_check_modules(HOWDY_TEST_MAIN ${query})
  return()
endif()

howdy_isolate_cross_pkg_config(HOWDY_TEST_MAIN)
howdy_pkg_check_modules(HOWDY_TEST_MAIN ${query})

if(DEFINED EXPECTED_VERSION AND NOT HOWDY_TEST_MAIN_VERSION STREQUAL EXPECTED_VERSION)
  message(FATAL_ERROR "Expected version ${EXPECTED_VERSION}, got ${HOWDY_TEST_MAIN_VERSION}")
endif()

if(DEFINED EXPECTED_INCLUDE_DIR AND NOT HOWDY_TEST_MAIN_INCLUDE_DIRS STREQUAL EXPECTED_INCLUDE_DIR)
  message(
    FATAL_ERROR
    "Expected include dir ${EXPECTED_INCLUDE_DIR}, got ${HOWDY_TEST_MAIN_INCLUDE_DIRS}"
  )
endif()
