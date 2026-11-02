install(SCRIPT "${PROJECT_BINARY_DIR}/cmake_install_prefix_guard.cmake")
install(SCRIPT "${PROJECT_BINARY_DIR}/install_preflight-$<CONFIG>.cmake")
add_subdirectory(po)

# Runtime config must only be loaded from the explicit path inside PAM.
add_library(pam_explicit_config_path INTERFACE)
target_compile_definitions(
 pam_explicit_config_path INTERFACE HOWDY_RUNTIME_CONFIG_EXPLICIT_PATH_ONLY
)

add_library(pam_options STATIC src/module/pam_options.cpp)
howdy_configure_pam_target(pam_options)
target_link_libraries(pam_options PRIVATE howdy_pam_option_catalog)

add_library(pam_message_locale STATIC src/runtime/message_locale.cpp)
howdy_configure_pam_target(pam_message_locale)

add_library(
 pam_runtime STATIC
 src/runtime/auth_helper_process.cpp
 src/runtime/auth_helper_process/internal.hpp
 src/runtime/auth_helper_process/io.cpp
 src/runtime/auth_helper_process/lease.cpp
 src/runtime/auth_helper_process/spawn.cpp
 src/runtime/compare_process.cpp
 src/runtime/environment.cpp
 src/runtime/lid_probe.cpp
 src/runtime/runtime_session.cpp
 src/runtime/session_probe.cpp
)
howdy_configure_pam_target(pam_runtime)
target_link_libraries(
 pam_runtime
 PUBLIC PAM::PAM
 PRIVATE howdy_runtime_config_loader pam_explicit_config_path
)

add_library(
 pam_conversation_response STATIC
 src/prompt/conversation_response.cpp
)
howdy_configure_pam_target(pam_conversation_response)
target_link_libraries(pam_conversation_response PUBLIC PAM::PAM)

add_library(
 pam_prompt_core STATIC
 src/prompt/internal_fd.cpp
 src/prompt/prompt_coordinator.cpp
 src/prompt/workaround.cpp
)
howdy_configure_pam_target(pam_prompt_core)
target_link_libraries(pam_prompt_core PUBLIC PAM::PAM PRIVATE Threads::Threads)

add_library(
 pam_observed_prompt_conversation STATIC
 src/prompt/observed_prompt_conversation.cpp
 src/prompt/conversation_restore/internal.hpp
)
howdy_configure_pam_target(pam_observed_prompt_conversation)
target_link_libraries(
 pam_observed_prompt_conversation
 PUBLIC PAM::PAM
 PRIVATE pam_conversation_response
)

add_library(
 pam_prompt STATIC
 src/prompt/prompt_submitter.cpp
 src/prompt/pam_conversation.cpp
 src/prompt/native_prompt_conversation.cpp
 src/prompt/native_prompt_input.cpp
)
howdy_configure_pam_target(pam_prompt)
target_link_libraries(
 pam_prompt
 PUBLIC PAM::PAM
 PRIVATE Threads::Threads PkgConfig::LIBEVDEV pam_prompt_core
         pam_conversation_response pam_observed_prompt_conversation
)

add_library(
 pam_module STATIC
 src/module/auth_eligibility.cpp
 src/module/auth_flow.cpp
 src/module/confirmation_message.cpp
 src/module/status_mapping.cpp
)
howdy_configure_pam_target(pam_module)
target_compile_definitions(
 pam_module
 PRIVATE
  LOCALEDIR="${CMAKE_INSTALL_FULL_LOCALEDIR}"
)
target_link_libraries(
 pam_module
 PUBLIC PAM::PAM
 PRIVATE pam_options pam_prompt pam_runtime howdy_user_model_readiness
         pam_explicit_config_path
)

add_library(pam_entrypoint STATIC src/module/entrypoint.cpp)
howdy_configure_pam_target(pam_entrypoint)
target_link_libraries(
 pam_entrypoint
 PUBLIC PAM::PAM
 PRIVATE pam_message_locale
)

add_library(
 pam_production_entrypoint STATIC
 src/module/production_entrypoint.cpp
)
howdy_configure_pam_target(pam_production_entrypoint)
target_link_libraries(
 pam_production_entrypoint
 PUBLIC pam_entrypoint
 PRIVATE pam_module pam_prompt pam_runtime
)

# OBJECT so the pam_sm_* symbols are always linked (nothing references them).
add_library(
 pam_unsupported_entrypoints OBJECT
 src/module/main/unsupported_entrypoints.cpp
)
howdy_configure_pam_target(pam_unsupported_entrypoints)
target_link_libraries(pam_unsupported_entrypoints PRIVATE PAM::PAM)

add_library(pam_howdy MODULE src/module/main.cpp)
howdy_configure_pam_target(pam_howdy)
target_link_libraries(
 pam_howdy
 PRIVATE pam_production_entrypoint pam_unsupported_entrypoints
         pam_explicit_config_path
)
target_link_options(pam_howdy PRIVATE "LINKER:--exclude-libs,ALL")
set_target_properties(pam_howdy PROPERTIES PREFIX "" OUTPUT_NAME "pam_howdy")
howdy_install(
 TARGETS pam_howdy
 LIBRARY DESTINATION "${HOWDY_PAM_DIR}"
)

if(BUILD_TESTING)
  include(CheckCXXSourceCompiles)
  cmake_push_check_state(RESET)
  set(CMAKE_REQUIRED_LIBRARIES PAM::PAM)
  check_cxx_source_compiles(
  [=[
   #include <security/pam_appl.h>
   int main() {
    pam_handle_t *handle = nullptr;
    pam_conv conversation{};
    return pam_start_confdir(
     "howdy-test", "nobody", &conversation, "/tmp", &handle);
   }
  ]=]
  HOWDY_HAVE_PAM_START_CONFDIR
 )
  cmake_pop_check_state()

  function(howdy_add_pam_test target test_name)
    add_executable(${target} ${ARGN})
    howdy_configure_pam_test_target(${target})
    if(HOWDY_HAVE_PAM_START_CONFDIR)
      target_compile_definitions(${target} PRIVATE HOWDY_HAVE_PAM_START_CONFDIR)
    endif()
    add_test(NAME ${test_name} COMMAND ${target})
  endfunction()

  include(cmake/tests/module.cmake)
  include(cmake/tests/runtime.cmake)
  include(cmake/tests/prompt.cmake)
  include(cmake/tests/integration.cmake)
endif()
