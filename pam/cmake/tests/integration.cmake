set(pam_integration_dir "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration")
add_executable(pam_installed_path_validator
               tests/integration/installed_pam_path_validator.cpp)
howdy_configure_pam_test_target(pam_installed_path_validator)
add_executable(pam_installed_state_verifier
               tests/integration/installed_pam_state_verifier.cpp)
howdy_configure_pam_test_target(pam_installed_state_verifier)
target_link_libraries(pam_installed_state_verifier PRIVATE howdy_runtime_descriptor_message)
add_test(
 NAME pam-installed-setuid-path-validation
 COMMAND pam_installed_path_validator --self-test
)
add_test(NAME pam-installed-setuid-state-verifier
 COMMAND pam_installed_state_verifier --self-test)
howdy_set_test_timeouts()

if(HOWDY_ENABLE_PRIVILEGED_TESTS)
  set(
   pam_e2e_artifact_targets
   howdy
   howdy_compare
   howdy_auth_helper
   pam_howdy
   howdy_packaged_config
   howdy_docs
   howdy-gmo
   pam_installed_path_validator
   pam_installed_state_verifier
  )
  if(HOWDY_HAVE_PAM_START_CONFDIR)
    add_executable(
   pam_installed_e2e_driver
   tests/integration/installed_pam_driver.cpp
  )
    howdy_configure_pam_target(pam_installed_e2e_driver)
    target_link_libraries(pam_installed_e2e_driver PRIVATE PAM::PAM)
    add_executable(
   pam_installed_compare_e2e_probe
   tests/integration/installed_compare_e2e_probe.cpp
  )
    howdy_configure_pam_target(pam_installed_compare_e2e_probe)
    add_executable(
   pam_installed_runtime_e2e_driver
   tests/integration/installed_runtime_e2e_driver.cpp
  )
    howdy_configure_pam_target(pam_installed_runtime_e2e_driver)
    add_executable(pam_installed_install_environment_test
   tests/integration/installed_pam_install_environment_test.cpp)
    howdy_configure_pam_test_target(pam_installed_install_environment_test)
    target_link_libraries(
   pam_installed_runtime_e2e_driver
   PRIVATE pam_runtime howdy_user_models
  )
    list(
     APPEND pam_e2e_artifact_targets
     pam_installed_e2e_driver
     pam_installed_compare_e2e_probe
     pam_installed_runtime_e2e_driver
     pam_installed_install_environment_test
    )
    # Each argument stays quoted so an empty directory keeps its position.
    function(pam_add_installed_e2e_test test_name runner)
      add_test(
        NAME ${test_name}
        COMMAND
          "${runner}"
          "${pam_integration_dir}/installed_pam_setuid_e2e.sh"
          "$<TARGET_FILE:pam_installed_path_validator>"
          "$<TARGET_FILE:pam_installed_state_verifier>"
          "${PROJECT_BINARY_DIR}"
          "${CMAKE_INSTALL_PREFIX}"
          "${HOWDY_PAM_DIR}"
          "${HOWDY_HELPER_DIR}"
          "${HOWDY_CONFIG_DIR}"
          "${HOWDY_USER_MODELS_DIR}"
          "$<TARGET_FILE:pam_installed_e2e_driver>"
          "$<TARGET_FILE:pam_installed_runtime_e2e_driver>"
          "$<TARGET_FILE:pam_installed_compare_e2e_probe>"
      )
    endfunction()
    pam_add_installed_e2e_test(installed-pam-setuid-e2e bash)
    pam_add_installed_e2e_test(
      installed-pam-setuid-install-environment
      pam_installed_install_environment_test
    )
  else()
    add_test(
   NAME installed-pam-setuid-e2e
   COMMAND bash -c "echo 'SKIP: pam_start_confdir unavailable'; exit 77"
  )
    add_test(
   NAME installed-pam-setuid-install-environment
   COMMAND bash -c "echo 'SKIP: pam_start_confdir unavailable'; exit 77"
  )
  endif()
  add_custom_target(
   howdy-installed-e2e-artifacts
   DEPENDS ${pam_e2e_artifact_targets}
  )
  set_tests_properties(
  installed-pam-setuid-e2e installed-pam-setuid-install-environment
  PROPERTIES
   RUN_SERIAL TRUE
   LABELS "privileged;e2e;pam;setuid"
   SKIP_RETURN_CODE 77
   TIMEOUT 180
 )
endif()
