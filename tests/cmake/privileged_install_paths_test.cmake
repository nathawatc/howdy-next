cmake_minimum_required(VERSION 3.31)

set(howdy_source_dir "${CMAKE_CURRENT_LIST_DIR}/../..")
include("${howdy_source_dir}/cmake/howdy_privileged_test_paths.cmake")

set(howdy_prefix "/opt/howdy-installed-pam-e2e-configure-test")
set(howdy_bindir "${howdy_prefix}/bin")
set(howdy_datadir "${howdy_prefix}/share")
set(howdy_localedir "${howdy_prefix}/share/locale")
set(howdy_mandir "${howdy_prefix}/share/man")
set(howdy_pam_dir "${howdy_prefix}/lib/security")
set(howdy_config_dir "${howdy_prefix}/etc/howdy")
set(howdy_models_dir "${howdy_prefix}/share/howdy/models")
set(howdy_user_models_dir "${howdy_prefix}/etc/howdy/models")
set(howdy_log_path "${howdy_prefix}/var/log/howdy")
set(howdy_helper_dir "${howdy_prefix}/libexec/howdy")
set(howdy_auth_helper_path "${howdy_helper_dir}/howdy-auth-helper")
set(howdy_licenses_install_dir "share/licenses/howdy")

if(DEFINED HOWDY_PRIVILEGED_TEST_CASE)
  if(HOWDY_PRIVILEGED_TEST_CASE STREQUAL "bindir")
    set(howdy_bindir "/usr/bin")
  elseif(HOWDY_PRIVILEGED_TEST_CASE STREQUAL "datadir")
    set(howdy_datadir "/usr/share")
  elseif(HOWDY_PRIVILEGED_TEST_CASE STREQUAL "localedir")
    set(howdy_localedir "/usr/share/locale")
  elseif(HOWDY_PRIVILEGED_TEST_CASE STREQUAL "mandir")
    set(howdy_mandir "/usr/share/man")
  elseif(HOWDY_PRIVILEGED_TEST_CASE STREQUAL "licenses")
    set(howdy_licenses_install_dir "/usr/share/licenses/howdy")
  elseif(HOWDY_PRIVILEGED_TEST_CASE STREQUAL "pam_dir")
    set(howdy_pam_dir "/lib/security")
  elseif(HOWDY_PRIVILEGED_TEST_CASE STREQUAL "config_dir")
    set(howdy_config_dir "/etc/howdy")
  elseif(HOWDY_PRIVILEGED_TEST_CASE STREQUAL "models_dir")
    set(howdy_models_dir "/usr/share/howdy/models")
  elseif(HOWDY_PRIVILEGED_TEST_CASE STREQUAL "user_models_dir")
    set(howdy_user_models_dir "/etc/howdy/models")
  elseif(HOWDY_PRIVILEGED_TEST_CASE STREQUAL "log_path")
    set(howdy_log_path "/var/log/howdy")
  elseif(HOWDY_PRIVILEGED_TEST_CASE STREQUAL "helper_dir")
    set(howdy_helper_dir "/usr/libexec/howdy")
  elseif(HOWDY_PRIVILEGED_TEST_CASE STREQUAL "auth_helper_path")
    set(howdy_auth_helper_path "/usr/libexec/howdy/howdy-auth-helper")
  elseif(HOWDY_PRIVILEGED_TEST_CASE STREQUAL "traversal")
    set(howdy_config_dir "${howdy_prefix}/../../etc/howdy")
  elseif(HOWDY_PRIVILEGED_TEST_CASE STREQUAL "prefix")
    set(howdy_prefix "/usr")
  elseif(HOWDY_PRIVILEGED_TEST_CASE STREQUAL "prefix_traversal")
    set(howdy_prefix "/opt/howdy-installed-pam-e2e-x/../../etc")
  endif()

  howdy_validate_privileged_test_paths(
  CMAKE_INSTALL_PREFIX "${howdy_prefix}"
  CMAKE_INSTALL_BINDIR "${howdy_bindir}"
  CMAKE_INSTALL_DATADIR "${howdy_datadir}"
  CMAKE_INSTALL_LOCALEDIR "${howdy_localedir}"
  CMAKE_INSTALL_MANDIR "${howdy_mandir}"
  HOWDY_PAM_DIR "${howdy_pam_dir}"
  HOWDY_CONFIG_DIR "${howdy_config_dir}"
  HOWDY_MODELS_DIR "${howdy_models_dir}"
  HOWDY_USER_MODELS_DIR "${howdy_user_models_dir}"
  HOWDY_LOG_PATH "${howdy_log_path}"
  HOWDY_HELPER_DIR "${howdy_helper_dir}"
  HOWDY_AUTH_HELPER_PATH "${howdy_auth_helper_path}"
  HOWDY_LICENSES_INSTALL_DIR "${howdy_licenses_install_dir}"
 )
else()
  set(rejected_cases
  bindir
  datadir
  localedir
  mandir
  licenses
  pam_dir
  config_dir
  models_dir
  user_models_dir
  log_path
  helper_dir
  auth_helper_path
  traversal
 )
  set(rejected_paths
  /usr/bin
  /usr/share
  /usr/share/locale
  /usr/share/man
  /usr/share/licenses/howdy
  /lib/security
  /etc/howdy
  /usr/share/howdy/models
  /etc/howdy/models
  /var/log/howdy
  /usr/libexec/howdy
  /usr/libexec/howdy/howdy-auth-helper
  /opt/howdy-installed-pam-e2e-configure-test/../../etc/howdy
 )
  list(LENGTH rejected_cases rejected_case_count)
  math(EXPR rejected_last_index "${rejected_case_count} - 1")
  foreach(rejected_index RANGE "${rejected_last_index}")
    list(GET rejected_cases "${rejected_index}" rejected_case)
    list(GET rejected_paths "${rejected_index}" rejected_path)
    execute_process(
   COMMAND
    "${CMAKE_COMMAND}"
    "-DHOWDY_PRIVILEGED_TEST_CASE=${rejected_case}"
    -P "${CMAKE_CURRENT_LIST_FILE}"
   RESULT_VARIABLE rejected_result
   OUTPUT_VARIABLE rejected_output
   ERROR_VARIABLE rejected_error
  )
    if(rejected_result EQUAL 0)
      message(
    FATAL_ERROR
    "Escaped install destination unexpectedly passed: "
    "${rejected_case}=${rejected_path}"
   )
    endif()
    # CMake wraps long diagnostics, so compare with whitespace runs collapsed.
    string(REGEX REPLACE "[ \t\r\n]+" " " rejected_text "${rejected_output} ${rejected_error}")
    string(
   FIND
   "${rejected_text}"
   "Privileged-test path escapes isolated prefix: ${rejected_path}"
   rejected_message
  )
    if(rejected_message EQUAL -1)
      message(
    FATAL_ERROR
    "Escaped install destination omitted isolation diagnostic: "
    "${rejected_case}=${rejected_path}\n${rejected_output}\n${rejected_error}"
   )
    endif()
  endforeach()

  foreach(prefix_case IN ITEMS prefix prefix_traversal)
    execute_process(
   COMMAND
    "${CMAKE_COMMAND}"
    "-DHOWDY_PRIVILEGED_TEST_CASE=${prefix_case}"
    -P "${CMAKE_CURRENT_LIST_FILE}"
   RESULT_VARIABLE prefix_result
   OUTPUT_VARIABLE prefix_output
   ERROR_VARIABLE prefix_error
  )
    string(
   FIND
   "${prefix_output}\n${prefix_error}"
   "Privileged tests require an isolated CMAKE_INSTALL_PREFIX"
   prefix_message
  )
    if(prefix_result EQUAL 0 OR prefix_message EQUAL -1)
      message(
    FATAL_ERROR
    "Non-isolated install prefix not rejected: ${prefix_case}\n"
    "${prefix_output}\n${prefix_error}"
   )
    endif()
  endforeach()

  execute_process(
  COMMAND
   "${CMAKE_COMMAND}"
   -DHOWDY_PRIVILEGED_TEST_CASE=valid
   -P "${CMAKE_CURRENT_LIST_FILE}"
  RESULT_VARIABLE accepted_result
  OUTPUT_VARIABLE accepted_output
  ERROR_VARIABLE accepted_error
 )
  if(NOT accepted_result EQUAL 0)
    message(
   FATAL_ERROR
   "Valid isolated install destinations failed:\n"
   "${accepted_output}\n${accepted_error}"
  )
  endif()
endif()
