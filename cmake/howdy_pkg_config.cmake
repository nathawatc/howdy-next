# Fail-closed pkg-config lookups.
#
# CMake 4.5 checks module existence with `pkg-config --modversion`, which pkgconf answers without
# resolving `Requires`, and then ignores failing --cflags/--libs queries.
# pkg_check_modules(REQUIRED) can therefore succeed with empty flags for a package whose
# dependencies are missing (issue #15).
# howdy_pkg_check_modules() first runs `pkg-config --cflags --libs` on the modules, failing
# configure when that query fails, and then runs pkg_check_modules() for the results.

# Mirrors the module-spec split in FindPkgConfig: "name", "name>=1.0", "name >= 1.0".
function(_howdy_pkg_config_query out spec)
  if(spec MATCHES "(.*[^>< \t])[ \t]*(=|[><]=?)[ \t]*(.*)")
    set(query "${CMAKE_MATCH_1} ${CMAKE_MATCH_2} ${CMAKE_MATCH_3}")
  else()
    set(query "${spec}")
  endif()
  set(${out} "${query}" PARENT_SCOPE)
endfunction()

function(_howdy_require_pkg_config_resolvable prefix)
  # FindPkgConfig's path helpers read these two names (stable since CMake 3.1).
  set(_no_cmake_path 0)
  set(_no_cmake_environment_path 0)
  if(DEFINED PKG_CONFIG_USE_CMAKE_PREFIX_PATH AND NOT PKG_CONFIG_USE_CMAKE_PREFIX_PATH)
    set(_no_cmake_path 1)
    set(_no_cmake_environment_path 1)
  endif()

  set(queries)
  foreach(arg IN LISTS ARGN)
    if(arg STREQUAL "NO_CMAKE_PATH")
      set(_no_cmake_path 1)
    elseif(arg STREQUAL "NO_CMAKE_ENVIRONMENT_PATH")
      set(_no_cmake_environment_path 1)
    elseif(NOT arg MATCHES "^(REQUIRED|QUIET|IMPORTED_TARGET|GLOBAL)$")
      _howdy_pkg_config_query(query "${arg}")
      list(APPEND queries "${query}")
    endif()
  endforeach()

  # Search the same CMAKE_PREFIX_PATH-derived directories as pkg_check_modules().
  set(use_find_pkg_config_paths FALSE)
  if(COMMAND _pkg_set_path_internal AND COMMAND _pkg_restore_path_internal)
    set(use_find_pkg_config_paths TRUE)
    _pkg_set_path_internal()
  elseif(NOT (_no_cmake_path AND _no_cmake_environment_path))
    message(
      WARNING
      "FindPkgConfig path helpers are unavailable; the pkg-config check for ${prefix} does not "
      "search CMAKE_PREFIX_PATH"
    )
  endif()

  # The same flag query pkg_check_modules() runs, for all modules at once; FindPkgConfig 4.5
  # ignores its failure, so check it here.
  execute_process(
    COMMAND ${PKG_CONFIG_EXECUTABLE} ${PKG_CONFIG_ARGN} --print-errors --short-errors --cflags
            --libs ${queries}
    RESULT_VARIABLE result
    OUTPUT_QUIET
    ERROR_VARIABLE error
    ERROR_STRIP_TRAILING_WHITESPACE
  )

  if(use_find_pkg_config_paths)
    _pkg_restore_path_internal()
  endif()

  if(NOT result EQUAL 0)
    list(JOIN queries ", " modules)
    message(
      FATAL_ERROR
      "pkg-config cannot resolve the modules required for ${prefix} (${modules}):\n${error}"
    )
  endif()
endfunction()

# howdy_pkg_check_modules(<prefix> [IMPORTED_TARGET [GLOBAL]] [NO_CMAKE_PATH]
#                         [NO_CMAKE_ENVIRONMENT_PATH] <module-spec>...)
# Same as pkg_check_modules(<prefix> REQUIRED ...), but fails before the lookup when the modules'
# Requires cannot be resolved. A macro keeps pkg_check_modules() results in the caller's scope.
macro(howdy_pkg_check_modules prefix)
  _howdy_require_pkg_config_resolvable(${prefix} ${ARGN})
  # A build tree configured by plain pkg_check_modules() on CMake 4.5 may have cached a lookup
  # with empty flags. Re-query once per build tree; later configures use FindPkgConfig's cache.
  if(NOT _howdy_pkg_config_checked_${prefix})
    unset(__pkg_config_checked_${prefix} CACHE)
  endif()
  pkg_check_modules(${prefix} REQUIRED ${ARGN})
  set(_howdy_pkg_config_checked_${prefix} TRUE CACHE INTERNAL "")
endmacro()
