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
  cmake_parse_arguments(ARG "EXPECT_FAILURE;SEED" "LIBDIR;SYSROOT;CMAKE_SYSROOT;EXPECTED_VERSION;EXPECTED_INCLUDE_DIR;QUERY" "ROOTS" ${ARGN})

  set(cmd "${CMAKE_COMMAND}")
  if(DEFINED ARG_LIBDIR)
    set(cmd "${CMAKE_COMMAND}" -E env "PKG_CONFIG_LIBDIR=${ARG_LIBDIR}" "PKG_CONFIG_PATH=${host_pc_dir}" "PKG_CONFIG_SYSROOT_DIR=${ARG_SYSROOT}" "${CMAKE_COMMAND}")
  else()
    set(cmd "${CMAKE_COMMAND}" -E env "PKG_CONFIG_PATH=${host_pc_dir}" "PKG_CONFIG_SYSROOT_DIR=${ARG_SYSROOT}" "${CMAKE_COMMAND}")
  endif()

  set(extra_args)
  if(DEFINED ARG_CMAKE_SYSROOT)
    list(APPEND extra_args "-DCMAKE_SYSROOT=${ARG_CMAKE_SYSROOT}")
  endif()
  if(DEFINED ARG_ROOTS)
    list(APPEND extra_args "-DCMAKE_FIND_ROOT_PATH=${ARG_ROOTS}")
  endif()
  if(DEFINED ARG_EXPECTED_VERSION)
    list(APPEND extra_args "-DEXPECTED_VERSION=${ARG_EXPECTED_VERSION}")
  endif()
  if(DEFINED ARG_EXPECTED_INCLUDE_DIR)
    list(APPEND extra_args "-DEXPECTED_INCLUDE_DIR=${ARG_EXPECTED_INCLUDE_DIR}")
  endif()
  if(DEFINED ARG_QUERY)
    list(APPEND extra_args "-DHOWDY_TEST_QUERY=${ARG_QUERY}")
  endif()
  if(ARG_SEED)
    list(APPEND extra_args "-DHOWDY_SEED_HOST_CACHE=TRUE")
  endif()

  execute_process(
    COMMAND ${cmd}
      -S "${HOWDY_SOURCE_DIR}/tests/cmake/pkg_config_fixture"
      -B "${HOWDY_TEST_ROOT}/${name}"
      -G "${HOWDY_CMAKE_GENERATOR}"
      -DCMAKE_SYSTEM_NAME=Linux
      "-DHOWDY_SOURCE_DIR=${HOWDY_SOURCE_DIR}"
      ${extra_args}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
  )

  if(ARG_EXPECT_FAILURE)
    if(result EQUAL 0)
      message(FATAL_ERROR "Case '${name}' expected configure failure, but succeeded:\n${output}\n${error}")
    endif()
  else()
    if(NOT result EQUAL 0)
      message(FATAL_ERROR "Case '${name}' failed:\n${output}\n${error}")
    endif()
  endif()
endfunction()

# Hostile host PKG_CONFIG_PATH cannot supply missing target transitive dependency
run_configure(hostile-host-override LIBDIR "${target_pc_dir}" EXPECT_FAILURE)

# Supply valid target transitive dependency
file(WRITE "${target_pc_dir}/howdy-test-transitive.pc" [=[
Name: howdy-test-transitive
Description: target transitive package
Version: 2.0
]=])

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
  EXPECT_FAILURE)

# Missing target configuration (no LIBDIR, no sysroot/find root) fails explicitly
run_configure(missing-target-configuration EXPECT_FAILURE)

file(REMOVE_RECURSE "${HOWDY_TEST_ROOT}")
