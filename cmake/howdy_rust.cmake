include_guard(GLOBAL)

# Rust components are built by Cargo through Corrosion and linked into C++ as
# one staticlib (howdy-ffi). Only C++ FFI wrappers call howdy_link_rust_ffi();
# the raw C header stays outside the shared generated include root.

option(
 HOWDY_CARGO_OFFLINE
 "Build Rust crates from already-fetched or vendored sources only"
 OFF
)

find_package(Corrosion 0.5 REQUIRED)
find_program(HOWDY_CBINDGEN cbindgen REQUIRED)

set(howdy_cargo_manifest "${PROJECT_SOURCE_DIR}/Cargo.toml")
set(howdy_cargo_lock "${PROJECT_SOURCE_DIR}/Cargo.lock")
set(howdy_ffi_crate_dir "${PROJECT_SOURCE_DIR}/crates/howdy-ffi")

set(howdy_cargo_frozen)
set(howdy_cargo_fetch_args --locked)
set(howdy_cargo_env)
if(HOWDY_CARGO_OFFLINE)
  set(howdy_cargo_frozen FROZEN)
  list(APPEND howdy_cargo_fetch_args --offline)
  list(APPEND howdy_cargo_env CARGO_NET_OFFLINE=true)
endif()

corrosion_import_crate(
 MANIFEST_PATH "${howdy_cargo_manifest}"
 CRATES howdy-ffi
 CRATE_TYPES staticlib
 LOCKED
 ${howdy_cargo_frozen}
)

set(HOWDY_RUST_INCLUDE_DIR "${PROJECT_BINARY_DIR}/rust-include")
set(howdy_ffi_header "${HOWDY_RUST_INCLUDE_DIR}/howdy_ffi.h")
file(
 GLOB howdy_ffi_sources
 CONFIGURE_DEPENDS
 "${howdy_ffi_crate_dir}/src/*.rs"
)
add_custom_command(
 OUTPUT "${howdy_ffi_header}"
 COMMAND
  "${CMAKE_COMMAND}" -E env
  ${howdy_cargo_env}
  "CARGO_TARGET_DIR=${PROJECT_BINARY_DIR}/cargo-cbindgen"
  "${HOWDY_CBINDGEN}"
  --quiet
  --config "${howdy_ffi_crate_dir}/cbindgen.toml"
  --lockfile "${howdy_cargo_lock}"
  --output "${howdy_ffi_header}"
  "${howdy_ffi_crate_dir}"
 DEPENDS
  ${howdy_ffi_sources}
  "${howdy_ffi_crate_dir}/cbindgen.toml"
  "${howdy_ffi_crate_dir}/Cargo.toml"
  "${howdy_cargo_manifest}"
  "${howdy_cargo_lock}"
 COMMENT "Generating Rust FFI header"
 VERBATIM
)
add_custom_target(howdy_ffi_header DEPENDS "${howdy_ffi_header}")

# Everything clang-tidy needs before analysing sources without building them.
add_custom_target(howdy_generated_headers)
add_dependencies(howdy_generated_headers howdy_version_metadata howdy_ffi_header)

function(howdy_link_rust_ffi target)
  target_link_libraries(${target} PRIVATE howdy_ffi)
  target_include_directories(${target} PRIVATE "${HOWDY_RUST_INCLUDE_DIR}")
  add_dependencies(${target} howdy_ffi_header)
endfunction()

set(howdy_cargo_base_command
 "${CMAKE_COMMAND}" -E env
 ${howdy_cargo_env}
 "CARGO_TARGET_DIR=${PROJECT_BINARY_DIR}/cargo-check"
 "${Rust_CARGO_CACHED}"
)

# Needs network access for the advisory database, so CI runs it explicitly
# instead of from CTest.
add_custom_target(
 howdy_cargo_deny
 COMMAND
  ${howdy_cargo_base_command} deny
  --manifest-path "${howdy_cargo_manifest}"
  --locked
  check
 WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
 VERBATIM
)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
  find_program(HOWDY_CARGO_CLIPPY cargo-clippy REQUIRED)
  find_program(HOWDY_RUSTFMT rustfmt REQUIRED)

  add_test(
  NAME rust-cargo-test
  COMMAND
   ${howdy_cargo_base_command} test
   --workspace
   --manifest-path "${howdy_cargo_manifest}"
   ${howdy_cargo_fetch_args}
 )
  add_test(
  NAME rust-clippy
  COMMAND
   ${howdy_cargo_base_command} clippy
   --workspace
   --all-targets
   --manifest-path "${howdy_cargo_manifest}"
   ${howdy_cargo_fetch_args}
   -- -D warnings
 )
  add_test(
  NAME rust-fmt
  COMMAND
   ${howdy_cargo_base_command} fmt
   --all
   --manifest-path "${howdy_cargo_manifest}"
   --check
 )
  set_tests_properties(
  rust-cargo-test
  rust-clippy
  rust-fmt
  PROPERTIES
   RESOURCE_LOCK cargo
   TIMEOUT 600
 )
endif()
