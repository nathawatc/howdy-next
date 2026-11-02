cmake_minimum_required(VERSION 3.31)

# Pins howdy_resolve_install_paths() across the library layouts and prefixes that
# Arch (lib), Fedora (lib64), and Debian/Ubuntu (lib/<multiarch>) use, so a
# distro port cannot silently install the PAM module where libpam never looks.

foreach(required_variable IN ITEMS
 HOWDY_SOURCE_DIR
 HOWDY_TEST_ROOT
 HOWDY_CMAKE_GENERATOR
 HOWDY_RUN_HOST_NATIVE_CASE
)
  if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
    message(FATAL_ERROR "Missing install-paths test variable: ${required_variable}")
  endif()
endforeach()

file(REMOVE_RECURSE "${HOWDY_TEST_ROOT}")
file(MAKE_DIRECTORY "${HOWDY_TEST_ROOT}")

set_property(GLOBAL PROPERTY HOWDY_MATRIX_FAILURES "")

function(record_failure message)
  set_property(GLOBAL APPEND_STRING PROPERTY HOWDY_MATRIX_FAILURES "\n  ${message}")
endfunction()

# Configures the fixture for one case and compares each EXPECT_<NAME> value with
# the resolved RESOLVED_HOWDY_<NAME> value. Results are left in the caller's
# scope as RESOLVED_*.
function(run_case name)
  cmake_parse_arguments(
   PARSE_ARGV 1 case
   "HOST_NATIVE"
   "EXPECT_PAM_DIR;EXPECT_CONFIG_DIR;EXPECT_CONFIG_PATH;EXPECT_MODELS_DIR;EXPECT_USER_MODELS_DIR;EXPECT_LOG_PATH;EXPECT_HELPER_DIR;EXPECT_COMPARE_PROCESS_PATH;EXPECT_AUTH_HELPER_PATH"
   "DEFINES"
  )
  set(build_dir "${HOWDY_TEST_ROOT}/${name}")
  set(language_arguments "")
  if(case_HOST_NATIVE)
    set(language_arguments -DHOWDY_FIXTURE_LANGUAGES=CXX)
    if(CMAKE_CXX_COMPILER)
      list(APPEND language_arguments "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
    endif()
  endif()
  execute_process(
   COMMAND
    "${CMAKE_COMMAND}"
    -S "${HOWDY_SOURCE_DIR}/tests/cmake/install_paths_fixture"
    -B "${build_dir}"
    -G "${HOWDY_CMAKE_GENERATOR}"
    "-DHOWDY_SOURCE_DIR=${HOWDY_SOURCE_DIR}"
    ${language_arguments}
    ${case_DEFINES}
   RESULT_VARIABLE configure_result
   OUTPUT_VARIABLE configure_output
   ERROR_VARIABLE configure_error
  )
  if(NOT configure_result EQUAL 0)
    record_failure("[${name}] fixture configure failed (${configure_result}):\n${configure_output}\n${configure_error}")
    return()
  endif()
  include("${build_dir}/resolved.cmake")
  foreach(key IN ITEMS
   PAM_DIR CONFIG_DIR CONFIG_PATH MODELS_DIR USER_MODELS_DIR LOG_PATH HELPER_DIR
   COMPARE_PROCESS_PATH AUTH_HELPER_PATH
  )
    set(actual "${RESOLVED_HOWDY_${key}}")
    set("RESOLVED_HOWDY_${key}" "${actual}" PARENT_SCOPE)
    if(DEFINED case_EXPECT_${key} AND NOT actual STREQUAL case_EXPECT_${key})
      record_failure("[${name}] HOWDY_${key}: expected '${case_EXPECT_${key}}', got '${actual}'")
    endif()
  endforeach()
  set(RESOLVED_CMAKE_INSTALL_LIBDIR "${RESOLVED_CMAKE_INSTALL_LIBDIR}" PARENT_SCOPE)
endfunction()

# Everything except the PAM directory is independent of the library layout when
# the prefix is /usr (FHS: /etc, /usr/libexec, /usr/share).
set(usr_layout_expectations
 EXPECT_CONFIG_DIR /etc/howdy
 EXPECT_CONFIG_PATH /etc/howdy/config.ini
 EXPECT_USER_MODELS_DIR /etc/howdy/models
 EXPECT_MODELS_DIR /usr/share/howdy/models
 EXPECT_LOG_PATH /var/log/howdy
 EXPECT_HELPER_DIR /usr/libexec/howdy
 EXPECT_COMPARE_PROCESS_PATH /usr/libexec/howdy/howdy-compare
 EXPECT_AUTH_HELPER_PATH /usr/libexec/howdy/howdy-auth-helper
)

run_case(
 arch-lib
 DEFINES -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib
 ${usr_layout_expectations}
 EXPECT_PAM_DIR /usr/lib/security
)
run_case(
 fedora-lib64
 DEFINES -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib64
 ${usr_layout_expectations}
 EXPECT_PAM_DIR /usr/lib64/security
)
run_case(
 debian-multiarch-x86_64
 DEFINES -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib/x86_64-linux-gnu
 ${usr_layout_expectations}
 EXPECT_PAM_DIR /usr/lib/x86_64-linux-gnu/security
)
run_case(
 debian-multiarch-aarch64
 DEFINES -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib/aarch64-linux-gnu
 ${usr_layout_expectations}
 EXPECT_PAM_DIR /usr/lib/aarch64-linux-gnu/security
)

# The default CMAKE_INSTALL_PREFIX is /usr/local, where libpam does not look.
run_case(
 usr-local
 DEFINES -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_INSTALL_LIBDIR=lib
 EXPECT_PAM_DIR /usr/local/lib/security
 EXPECT_CONFIG_DIR /usr/local/etc/howdy
 EXPECT_CONFIG_PATH /usr/local/etc/howdy/config.ini
 EXPECT_USER_MODELS_DIR /usr/local/etc/howdy/models
 EXPECT_MODELS_DIR /usr/local/share/howdy/models
 EXPECT_LOG_PATH /var/log/howdy
 EXPECT_HELPER_DIR /usr/local/libexec/howdy
 EXPECT_AUTH_HELPER_PATH /usr/local/libexec/howdy/howdy-auth-helper
)

# Packager overrides win over every derived default. Overrides are absolute: CMake
# makes relative PATH cache values absolute against the working directory.
run_case(
 absolute-overrides
 DEFINES
  -DCMAKE_INSTALL_PREFIX=/usr
  -DCMAKE_INSTALL_LIBDIR=lib64
  -DHOWDY_PAM_DIR=/lib/security
  -DHOWDY_CONFIG_DIR=/etc/howdy.d
  -DHOWDY_MODELS_DIR=/var/lib/howdy/models
  -DHOWDY_LOG_PATH=/var/log/howdy-custom
  -DHOWDY_AUTH_HELPER_PATH=/run/wrappers/bin/howdy-auth-helper
 EXPECT_PAM_DIR /lib/security
 EXPECT_CONFIG_DIR /etc/howdy.d
 EXPECT_CONFIG_PATH /etc/howdy.d/config.ini
 EXPECT_USER_MODELS_DIR /etc/howdy.d/models
 EXPECT_MODELS_DIR /var/lib/howdy/models
 EXPECT_LOG_PATH /var/log/howdy-custom
 EXPECT_HELPER_DIR /usr/libexec/howdy
 EXPECT_COMPARE_PROCESS_PATH /usr/libexec/howdy/howdy-compare
 EXPECT_AUTH_HELPER_PATH /run/wrappers/bin/howdy-auth-helper
)
# Privileged end-to-end builds stay under the isolated prefix regardless of the
# library layout.
run_case(
 privileged-e2e-ignores-libdir
 DEFINES
  -DCMAKE_INSTALL_PREFIX=/opt/howdy-installed-pam-e2e-matrix
  -DCMAKE_INSTALL_LIBDIR=lib64
  -DBUILD_TESTING=ON
  -DHOWDY_ENABLE_PRIVILEGED_TESTS=ON
 EXPECT_PAM_DIR /opt/howdy-installed-pam-e2e-matrix/lib/security
 EXPECT_CONFIG_DIR /opt/howdy-installed-pam-e2e-matrix/etc/howdy
 EXPECT_USER_MODELS_DIR /opt/howdy-installed-pam-e2e-matrix/etc/howdy/models
 EXPECT_MODELS_DIR /opt/howdy-installed-pam-e2e-matrix/share/howdy/models
 EXPECT_LOG_PATH /opt/howdy-installed-pam-e2e-matrix/var/log/howdy
 EXPECT_HELPER_DIR /opt/howdy-installed-pam-e2e-matrix/libexec/howdy
)

# Host-native case: with a /usr prefix and no explicit libdir, GNUInstallDirs
# picks this machine's convention. The resolved PAM directory must be one that
# holds the system's own PAM modules, otherwise libpam would never load
# pam_howdy.so after a distro-style install.
if(HOWDY_RUN_HOST_NATIVE_CASE)
  run_case(
   host-native
   HOST_NATIVE
   DEFINES -DCMAKE_INSTALL_PREFIX=/usr
  )
  set(system_pam_directories "")
  foreach(directory IN ITEMS
   /usr/lib/security /usr/lib64/security /lib/security /lib64/security
  )
    if(EXISTS "${directory}/pam_unix.so")
      file(REAL_PATH "${directory}" real_directory)
      list(APPEND system_pam_directories "${real_directory}")
    endif()
  endforeach()
  file(GLOB multiarch_pam_modules "/usr/lib/*/security/pam_unix.so" "/lib/*/security/pam_unix.so")
  foreach(module IN LISTS multiarch_pam_modules)
    cmake_path(GET module PARENT_PATH directory)
    file(REAL_PATH "${directory}" real_directory)
    list(APPEND system_pam_directories "${real_directory}")
  endforeach()
  list(REMOVE_DUPLICATES system_pam_directories)

  if(system_pam_directories STREQUAL "")
    string(REPLACE "," ";" required_capabilities "$ENV{HOWDY_TEST_REQUIRE}")
    if("pam-system-dir" IN_LIST required_capabilities)
      record_failure("[host-native] FAIL: required capability 'pam-system-dir' unavailable: no system pam_unix.so found")
    else()
      message(STATUS "SKIP: no system pam_unix.so found; host PAM directory check not run")
    endif()
  else()
    file(REAL_PATH "${RESOLVED_HOWDY_PAM_DIR}" resolved_real_directory)
    if(NOT resolved_real_directory IN_LIST system_pam_directories)
      record_failure("[host-native] HOWDY_PAM_DIR '${RESOLVED_HOWDY_PAM_DIR}' (libdir '${RESOLVED_CMAKE_INSTALL_LIBDIR}') is not a system PAM module directory; found: ${system_pam_directories}")
    endif()
  endif()
endif()

get_property(failures GLOBAL PROPERTY HOWDY_MATRIX_FAILURES)
if(NOT failures STREQUAL "")
  message(FATAL_ERROR "Install path matrix failures:${failures}")
endif()
