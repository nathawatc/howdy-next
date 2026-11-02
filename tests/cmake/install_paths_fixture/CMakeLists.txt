cmake_minimum_required(VERSION 3.31)
if(NOT DEFINED HOWDY_FIXTURE_LANGUAGES)
  set(HOWDY_FIXTURE_LANGUAGES NONE)
endif()
project(howdy_install_paths_fixture LANGUAGES ${HOWDY_FIXTURE_LANGUAGES})

include(GNUInstallDirs)
include("${HOWDY_SOURCE_DIR}/cmake/howdy_install_paths.cmake")
howdy_resolve_install_paths()

set(resolved "")
foreach(name IN ITEMS
 CMAKE_INSTALL_LIBDIR
 HOWDY_PAM_DIR
 HOWDY_CONFIG_DIR
 HOWDY_CONFIG_PATH
 HOWDY_MODELS_DIR
 HOWDY_USER_MODELS_DIR
 HOWDY_LOG_PATH
 HOWDY_HELPER_DIR
 HOWDY_COMPARE_PROCESS_PATH
 HOWDY_AUTH_HELPER_PATH
)
  string(APPEND resolved "set(RESOLVED_${name} \"${${name}}\")\n")
endforeach()
file(WRITE "${CMAKE_BINARY_DIR}/resolved.cmake" "${resolved}")
