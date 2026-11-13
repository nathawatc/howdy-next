cmake_minimum_required(VERSION 3.31)

set(target_root "${HOWDY_TEST_ROOT}/target")
set(target_pc_dir "${target_root}/usr/lib/pkgconfig")
set(host_pc_dir "${HOWDY_TEST_ROOT}/host")

file(REMOVE_RECURSE "${HOWDY_TEST_ROOT}")
file(MAKE_DIRECTORY "${target_pc_dir}" "${host_pc_dir}")

# Target package metadata (v1.0), requires howdy-test-transitive >= 2.0
file(WRITE "${target_pc_dir}/howdy-test-main.pc" [=[
prefix=/usr
Name: howdy-test-main
Description: target main package
Version: 1.0
Requires: howdy-test-transitive >= 2.0
Cflags: -I${prefix}/include
Libs: -L${prefix}/lib -lhowdy-fixture
]=])

# Host package metadata (v9.0) with host transitive package
file(WRITE "${host_pc_dir}/howdy-test-main.pc" [=[
prefix=/usr
Name: howdy-test-main
Description: host main package
Version: 9.0
Cflags: -I/host/include
Libs: -L/host/lib -lhowdy-fixture
]=])
file(WRITE "${host_pc_dir}/howdy-test-transitive.pc" [=[
Name: howdy-test-transitive
Description: host transitive package
Version: 9.0
]=])

function(run_configure name)
  set(options EXPECT_FAILURE SEED NATIVE PLAIN)
  set(
    one_value_args
    LIBDIR
    SYSROOT
    CMAKE_SYSROOT
    EXPECTED_VERSION
    EXPECTED_INCLUDE_DIR
    QUERY
    EXPECTED_ERROR
    PKG_PATH
    PREFIX_PATH
  )
  cmake_parse_arguments(ARG "${options}" "${one_value_args}" "ROOTS" ${ARGN})

  set(pkg_path "${host_pc_dir}")
  if(DEFINED ARG_PKG_PATH)
    set(pkg_path "${ARG_PKG_PATH}")
  endif()

  set(env "PKG_CONFIG_PATH=${pkg_path}" "PKG_CONFIG_SYSROOT_DIR=${ARG_SYSROOT}")
  if(DEFINED ARG_LIBDIR)
    list(PREPEND env "PKG_CONFIG_LIBDIR=${ARG_LIBDIR}")
  endif()
  set(cmd "${CMAKE_COMMAND}" -E env ${env} "${CMAKE_COMMAND}")

  # Pass every per-run setting on every run: a -D value stays in the build tree's cache, so a
  # later run in the same directory would otherwise inherit it (for example the SEED early return).
  set(extra_args)
  set(
    settings
    CMAKE_SYSROOT
    CMAKE_SYSROOT
    CMAKE_FIND_ROOT_PATH
    ROOTS
    CMAKE_PREFIX_PATH
    PREFIX_PATH
    EXPECTED_VERSION
    EXPECTED_VERSION
    EXPECTED_INCLUDE_DIR
    EXPECTED_INCLUDE_DIR
    HOWDY_TEST_QUERY
    QUERY
  )
  while(settings)
    list(POP_FRONT settings variable argument)
    if(DEFINED ARG_${argument})
      list(APPEND extra_args "-D${variable}=${ARG_${argument}}")
    else()
      list(APPEND extra_args "-U${variable}")
    endif()
  endwhile()
  list(
    APPEND
    extra_args
    "-DHOWDY_TEST_PLAIN_LOOKUP=${ARG_PLAIN}"
    "-DHOWDY_SEED_HOST_CACHE=${ARG_SEED}"
  )
  if(NOT ARG_NATIVE)
    list(APPEND extra_args -DCMAKE_SYSTEM_NAME=Linux)
  endif()

  execute_process(
    COMMAND ${cmd}
      -S "${HOWDY_SOURCE_DIR}/tests/cmake/pkg_config_fixture"
      -B "${HOWDY_TEST_ROOT}/${name}"
      -G "${HOWDY_CMAKE_GENERATOR}"
      "-DHOWDY_SOURCE_DIR=${HOWDY_SOURCE_DIR}"
      ${extra_args}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
  )

  if(ARG_EXPECT_FAILURE)
    if(result EQUAL 0)
      message(
        FATAL_ERROR
        "Case '${name}' expected configure failure, but succeeded:\n"
        "${output}\n${error}"
      )
    endif()
    # Require the intended failure, not any configure error. CMake wraps long messages, so
    # compare with whitespace collapsed.
    if(NOT DEFINED ARG_EXPECTED_ERROR)
      message(FATAL_ERROR "Case '${name}' expects failure but sets no EXPECTED_ERROR")
    endif()
    string(REGEX REPLACE "[ \t\r\n]+" " " actual "${output} ${error}")
    string(REGEX REPLACE "[ \t\r\n]+" " " expected "${ARG_EXPECTED_ERROR}")
    string(FIND "${actual}" "${expected}" position)
    if(position EQUAL -1)
      message(
        FATAL_ERROR
        "Case '${name}' failed without the expected error '${ARG_EXPECTED_ERROR}':\n"
        "${output}\n${error}"
      )
    endif()
  else()
    if(NOT result EQUAL 0)
      message(FATAL_ERROR "Case '${name}' failed:\n${output}\n${error}")
    endif()
  endif()
endfunction()

# Native lookup fails when a required package's Requires cannot be resolved
set(empty_pc_dir "${HOWDY_TEST_ROOT}/empty")
file(MAKE_DIRECTORY "${empty_pc_dir}")
run_configure(native-missing-requires
  NATIVE
  LIBDIR "${target_pc_dir}"
  PKG_PATH "${empty_pc_dir}"
  EXPECT_FAILURE
  EXPECTED_ERROR "pkg-config cannot resolve the modules required for HOWDY_TEST_MAIN")

# Hostile host PKG_CONFIG_PATH cannot supply missing target transitive dependency
run_configure(hostile-host-override
  LIBDIR "${target_pc_dir}"
  EXPECT_FAILURE
  EXPECTED_ERROR "pkg-config cannot resolve the modules required for HOWDY_TEST_MAIN")

# Supply valid target transitive dependency
file(WRITE "${target_pc_dir}/howdy-test-transitive.pc" [=[
Name: howdy-test-transitive
Description: target transitive package
Version: 2.0
]=])

# Native lookup succeeds once Requires resolve
run_configure(native-resolved-requires
  NATIVE
  LIBDIR "${target_pc_dir}"
  PKG_PATH "${empty_pc_dir}"
  EXPECTED_VERSION "1.0")

# After a failed configure, the same build tree configures with the real flags once the missing
# dependency appears
run_configure(native-missing-requires
  NATIVE
  LIBDIR "${target_pc_dir}"
  PKG_PATH "${empty_pc_dir}"
  EXPECTED_VERSION "1.0"
  EXPECTED_INCLUDE_DIR "/usr/include")

# Native lookup resolves packages and Requires found only through CMAKE_PREFIX_PATH
run_configure(native-cmake-prefix-path
  NATIVE
  LIBDIR "${empty_pc_dir}"
  PKG_PATH "${empty_pc_dir}"
  PREFIX_PATH "${target_root}/usr"
  EXPECTED_VERSION "1.0")

# A build tree first configured by plain pkg_check_modules() is re-queried once by the wrapper;
# later configures keep FindPkgConfig's cached result
set(refresh_pc_dir "${HOWDY_TEST_ROOT}/refresh")
file(MAKE_DIRECTORY "${refresh_pc_dir}")
function(write_refresh_pc include_dir)
  file(
    WRITE "${refresh_pc_dir}/howdy-test-main.pc"
    "Name: howdy-test-main\nDescription: refreshed package\nVersion: 1.0\n"
    "Cflags: -I${include_dir}\n"
  )
endfunction()
write_refresh_pc(/opt/old/include)
run_configure(native-refresh NATIVE PLAIN LIBDIR "${refresh_pc_dir}" PKG_PATH "${empty_pc_dir}")
write_refresh_pc(/opt/new/include)
run_configure(native-refresh
  NATIVE
  LIBDIR "${refresh_pc_dir}"
  PKG_PATH "${empty_pc_dir}"
  EXPECTED_INCLUDE_DIR "/opt/new/include")
write_refresh_pc(/opt/newer/include)
run_configure(native-refresh
  NATIVE
  LIBDIR "${refresh_pc_dir}"
  PKG_PATH "${empty_pc_dir}"
  EXPECTED_INCLUDE_DIR "/opt/new/include")

# Valid target PKG_CONFIG_LIBDIR resolves target package metadata
run_configure(valid-target-libdir LIBDIR "${target_pc_dir}" EXPECTED_VERSION "1.0")

# CMAKE_SYSROOT sets PKG_CONFIG_SYSROOT_DIR and rewrites include paths
run_configure(cmake-sysroot
  CMAKE_SYSROOT "${target_root}"
  ROOTS "${target_root}"
  EXPECTED_VERSION "1.0"
  EXPECTED_INCLUDE_DIR "${target_root}/usr/include")

# Stale cached host pkg-config result is cleared upon cross isolation
run_configure(stale-cache-isolation LIBDIR "${host_pc_dir}" SEED)
run_configure(stale-cache-isolation LIBDIR "${target_pc_dir}" EXPECTED_VERSION "1.0")

# Empty target root isolates from host default pkg-config directories
set(empty_target_root "${HOWDY_TEST_ROOT}/empty_target")
file(MAKE_DIRECTORY "${empty_target_root}")
run_configure(empty-root-no-host-fallback
  ROOTS "${empty_target_root}"
  QUERY "libcurl"
  EXPECT_FAILURE
  EXPECTED_ERROR "pkg-config cannot resolve the modules required for HOWDY_TEST_MAIN")

# Missing target configuration (no LIBDIR, no sysroot/find root) fails explicitly
run_configure(missing-target-configuration
  EXPECT_FAILURE
  EXPECTED_ERROR "Cross-compiling requires CMAKE_SYSROOT")

file(REMOVE_RECURSE "${HOWDY_TEST_ROOT}")
