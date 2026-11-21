# Exercise security fixtures below an unrelated writable ancestor on every run,
# including builds outside /tmp. Each suite owns its fixture boundary below.
set(howdy_security_test_workdir
    "${CMAKE_CURRENT_BINARY_DIR}/world-writable-test-work")
file(MAKE_DIRECTORY "${howdy_security_test_workdir}")
file(
 CHMOD "${howdy_security_test_workdir}"
 PERMISSIONS
  OWNER_READ OWNER_WRITE OWNER_EXECUTE
  GROUP_READ GROUP_WRITE GROUP_EXECUTE
  WORLD_READ WORLD_WRITE WORLD_EXECUTE
)

howdy_add_native_test(
 howdy_file_security_test
 native-file-security
 tests/support/file_security_test.cpp
)

set_tests_properties(
 native-auth-helper
 native-config-utils
 native-runtime-config-load
 native-runtime-config
 native-model-file
 native-user-models
 native-clear-cli
 native-config-cli
 native-disable-cli
 native-download-models
 native-snapshot-writer
 native-file-security
 PROPERTIES WORKING_DIRECTORY "${howdy_security_test_workdir}"
)

set_tests_properties(
 native-runtime-config-load
 native-runtime-config
 PROPERTIES ENVIRONMENT "TMPDIR=${howdy_security_test_workdir}"
)

howdy_add_native_test(
 howdy_atomic_files_fallback_test
 native-atomic-files-fallback
 tests/support/atomic_files_fallback_test.cpp
)

howdy_add_native_test(
 howdy_fd_io_test
 native-fd-io
 tests/support/fd_io_test.cpp
)
target_link_options(howdy_fd_io_test PRIVATE "LINKER:--wrap=read"
                      "LINKER:--wrap=__read_chk" "LINKER:--wrap=write")

howdy_add_native_test(
 howdy_user_names_test
 native-user-names
 tests/support/user_names_test.cpp
)

howdy_add_native_test(
 howdy_passwd_lookup_test
 native-passwd-lookup
 tests/support/passwd_lookup_test.cpp
)

howdy_add_native_test(
 howdy_invoking_user_env_test
 native-invoking-user-env
 tests/support/invoking_user_env_test.cpp
)

howdy_add_native_test(
 howdy_wayland_display_test
 native-wayland-display
 tests/support/wayland_display_test.cpp
)

howdy_add_native_test(
 howdy_capture_device_path_test
 native-capture-device-path
 tests/support/capture_device_path_test.cpp
)

howdy_add_native_test(
 howdy_cpu_topology_test
 native-cpu-topology
 tests/support/cpu_topology_test.cpp
)

howdy_add_native_test(
 howdy_runtime_descriptor_message_test
 native-runtime-descriptor-message
 tests/protocol/runtime_descriptor_message_test.cpp
)
target_link_libraries(
 howdy_runtime_descriptor_message_test
 PRIVATE howdy_runtime_descriptor_message
)

# The fuzz entry, run over fixed inputs so every build exercises it.
howdy_add_native_test(
 howdy_runtime_descriptor_message_fuzz_replay
 native-runtime-descriptor-message-fuzz-replay
 tests/protocol/runtime_descriptor_message_fuzz.cpp
 tests/protocol/runtime_descriptor_message_fuzz_replay.cpp
)
target_link_libraries(
 howdy_runtime_descriptor_message_fuzz_replay
 PRIVATE howdy_runtime_descriptor_message
)

if(HOWDY_BUILD_FUZZERS)
  # The decoder is compiled in, so libFuzzer's coverage instrumentation reaches it.
  add_executable(
  howdy_runtime_descriptor_message_fuzzer
  tests/protocol/runtime_descriptor_message_fuzz.cpp
  src/protocol/runtime_descriptor_message.cpp
 )
  howdy_configure_native_test_target(howdy_runtime_descriptor_message_fuzzer)
  target_compile_options(
  howdy_runtime_descriptor_message_fuzzer
  PRIVATE -fsanitize=fuzzer,address,undefined
 )
  target_link_options(
  howdy_runtime_descriptor_message_fuzzer
  PRIVATE -fsanitize=fuzzer,address,undefined
 )
endif()
