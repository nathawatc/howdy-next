set(howdy_test_emulator)
if(CMAKE_CROSSCOMPILING)
  set(howdy_test_emulator ${CMAKE_CROSSCOMPILING_EMULATOR})
endif()

function(howdy_require_crosscompiling_emulator)
  if(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
    message(
      FATAL_ERROR
      "Cross-compiling Howdy requires CMAKE_CROSSCOMPILING_EMULATOR "
      "to run the packaged-config generator"
    )
  endif()
endfunction()

function(howdy_isolate_cross_pkg_config)
  if(NOT CMAKE_CROSSCOMPILING)
    return()
  endif()

  set(roots ${CMAKE_SYSROOT} ${CMAKE_FIND_ROOT_PATH})
  list(REMOVE_DUPLICATES roots)

  if("$ENV{PKG_CONFIG_LIBDIR}" STREQUAL "" AND NOT roots)
    message(
      FATAL_ERROR
      "Cross-compiling requires CMAKE_SYSROOT, CMAKE_FIND_ROOT_PATH, or "
      "PKG_CONFIG_LIBDIR to isolate target pkg-config metadata"
    )
  endif()

  # Respect user/toolchain PKG_CONFIG_SYSROOT_DIR, or use CMAKE_SYSROOT if available
  if(NOT "$ENV{PKG_CONFIG_SYSROOT_DIR}" STREQUAL "")
    # Target sysroot explicitly configured
  elseif(CMAKE_SYSROOT)
    set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")
  endif()

  # Respect user/toolchain PKG_CONFIG_LIBDIR, or derive from target roots
  if(NOT "$ENV{PKG_CONFIG_LIBDIR}" STREQUAL "")
    # Target search paths explicitly configured
  else()
    set(pc_dirs)
    foreach(root IN LISTS roots)
      foreach(lib IN ITEMS usr/lib usr/lib64 usr/share lib lib64)
        list(APPEND pc_dirs "${root}/${lib}/pkgconfig")
        if(CMAKE_LIBRARY_ARCHITECTURE)
          list(APPEND pc_dirs "${root}/${lib}/${CMAKE_LIBRARY_ARCHITECTURE}/pkgconfig")
        endif()
      endforeach()
    endforeach()
    list(REMOVE_DUPLICATES pc_dirs)
    list(JOIN pc_dirs ":" libdir)
    set(ENV{PKG_CONFIG_LIBDIR} "${libdir}")
  endif()

  # Prevent host search paths from contaminating target package discovery
  unset(ENV{PKG_CONFIG_PATH})
  set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH FALSE PARENT_SCOPE)

  # Clear stale FindPkgConfig cache entries across configure runs
  get_cmake_property(variables VARIABLES)
  foreach(prefix IN LISTS ARGN)
    foreach(variable IN LISTS variables)
      if(variable MATCHES "^(${prefix}_|pkgcfg_lib_${prefix}_|__pkg_config_checked_${prefix}$|__pkg_config_arguments_${prefix}$)")
        unset("${variable}" CACHE)
        unset("${variable}" PARENT_SCOPE)
      endif()
    endforeach()
  endforeach()
endfunction()
