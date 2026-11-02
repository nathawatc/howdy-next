set(howdy_integration_dir "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration")
add_test(NAME native-howdy-help COMMAND howdy --help)
add_test(NAME native-howdy-version COMMAND howdy version)
howdy_assert_binary_result_nonroot(native-howdy-nonroot-path howdy 1
 "This command requires root privileges.\nRun it again with sudo.\n" ""
 -U coverage-user list)
add_test(NAME native-howdy-empty COMMAND howdy)
add_executable(howdy_bash_completion_integration_test
               tests/integration/bash_completion_test.cpp)
howdy_configure_native_test_target(howdy_bash_completion_integration_test)
add_test(
 NAME native-howdy-bash-completion
 COMMAND
  howdy_bash_completion_integration_test
  "${howdy_integration_dir}/bash_completion_test.sh"
  "$<TARGET_FILE:howdy>"
  "${CMAKE_CURRENT_SOURCE_DIR}/completions/howdy"
  ${howdy_test_emulator}
)

if(HOWDY_INSTALL_AUTH_HELPER_SETUID)
  set(howdy_auth_helper_install_mode 4755)
else()
  set(howdy_auth_helper_install_mode 755)
endif()

add_executable(howdy_install_layout_integration_test
               tests/integration/install_layout_test.cpp)
howdy_configure_native_test_target(howdy_install_layout_integration_test)
add_test(
 NAME native-howdy-install-layout
 COMMAND
  howdy_install_layout_integration_test
  "${PROJECT_BINARY_DIR}"
  "${CMAKE_INSTALL_PREFIX}"
  "${CMAKE_INSTALL_BINDIR}"
  "${CMAKE_INSTALL_LIBEXECDIR}"
  "${CMAKE_INSTALL_DATADIR}"
  "${CMAKE_INSTALL_LOCALEDIR}"
  "${CMAKE_INSTALL_MANDIR}"
  "${HOWDY_HELPER_DIR}"
  "${HOWDY_PAM_DIR}"
  "${HOWDY_CONFIG_DIR}"
  "${HOWDY_CONFIG_PATH}"
  "${HOWDY_MODELS_DIR}"
  "${HOWDY_USER_MODELS_DIR}"
  "${HOWDY_LICENSES_INSTALL_DIR}"
  "${howdy_auth_helper_install_mode}"
)
add_test(
 NAME native-howdy-privileged-install-paths
 COMMAND
  "${CMAKE_COMMAND}"
  -P "${PROJECT_SOURCE_DIR}/tests/cmake/privileged_install_paths_test.cmake"
)
if(CMAKE_CROSSCOMPILING)
  set(howdy_run_host_native_install_paths_case OFF)
else()
  set(howdy_run_host_native_install_paths_case ON)
endif()
add_test(
 NAME native-install-paths-matrix
 COMMAND
  "${CMAKE_COMMAND}"
  "-DHOWDY_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
  "-DHOWDY_TEST_ROOT=${PROJECT_BINARY_DIR}/install-paths-matrix-test"
  "-DHOWDY_CMAKE_GENERATOR=${CMAKE_GENERATOR}"
  "-DHOWDY_RUN_HOST_NATIVE_CASE=${howdy_run_host_native_install_paths_case}"
  "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
  -P "${PROJECT_SOURCE_DIR}/tests/cmake/install_paths_matrix_test.cmake"
)
set_tests_properties(
 native-install-paths-matrix
 PROPERTIES
  TIMEOUT 120
)
howdy_add_native_test(
 howdy_distro_conformance_test
 native-distro-conformance
 tests/integration/distro_conformance_test.cpp
)
add_executable(howdy_docs_generator_integration_test
              tests/integration/docs_generator_test.cpp)
howdy_configure_native_test_target(howdy_docs_generator_integration_test)
add_test(
 NAME native-howdy-docs-generator
 COMMAND
  howdy_docs_generator_integration_test
  "$<TARGET_FILE:howdy_docs_generator>"
  "${PROJECT_BINARY_DIR}/docs-generator-test"
  ${howdy_test_emulator}
)
add_executable(howdy_docs_integration_test tests/integration/docs_test.cpp)
howdy_configure_native_test_target(howdy_docs_integration_test)
add_test(
 NAME native-howdy-docs
 COMMAND
  howdy_docs_integration_test
  "${PROJECT_BINARY_DIR}"
  "${PROJECT_SOURCE_DIR}"
  "${HOWDY_MAN1_PATH}"
  "${HOWDY_MAN5_PATH}"
  "${HOWDY_MAN8_PATH}"
  "${HOWDY_CONFIG_DIR}"
  "${HOWDY_CONFIG_PATH}"
  "${HOWDY_MODELS_DIR}"
  "${HOWDY_USER_MODELS_DIR}"
  "${HOWDY_AUTH_HELPER_PATH}"
)
