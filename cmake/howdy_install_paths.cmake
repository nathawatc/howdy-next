# HOWDY_PRIVATE_LIBDIR becomes the RUNPATH of the binaries that link a bundled OpenCV, so
# an unsafe value would let the loader search a directory the packager did not intend.
# Validated on the raw value: normalization would hide a `..` and `$ORIGIN` is rejected,
# because glibc treats it specially for setuid callers.
function(howdy_validate_private_libdir value)
  foreach(character IN ITEMS "$" ":" "\"" ";" "\\" " " "\t" "\n" "\r")
    string(FIND "${value}" "${character}" position)
    if(NOT position EQUAL -1)
      message(
       FATAL_ERROR
       "HOWDY_PRIVATE_LIBDIR '${value}' contains a character that is not allowed in a RUNPATH directory ($ : \" ; \\ or whitespace)"
      )
    endif()
  endforeach()
  if(NOT IS_ABSOLUTE "${value}")
    message(FATAL_ERROR "HOWDY_PRIVATE_LIBDIR '${value}' must be an absolute path")
  endif()
  if("${value}" MATCHES "(^|/)\\.\\.(/|$)")
    message(FATAL_ERROR "HOWDY_PRIVATE_LIBDIR '${value}' must not contain '..'")
  endif()
endfunction()

# Resolves every installed/runtime path Howdy bakes into paths.hpp. A macro, not a
# function, so the cache entries and results land in the caller scope. Needs
# GNUInstallDirs and a project() so CMAKE_INSTALL_FULL_* and PROJECT_BINARY_DIR exist.
macro(howdy_resolve_install_paths)
  set(HOWDY_PAM_DIR "" CACHE PATH "pam_howdy destination directory")
  set(HOWDY_CONFIG_DIR "" CACHE PATH "Howdy configuration directory")
  set(HOWDY_MODELS_DIR "" CACHE PATH "OpenCV ONNX models directory")
  set(HOWDY_USER_MODELS_DIR "" CACHE PATH "User models directory")
  set(HOWDY_LOG_PATH "/var/log/howdy" CACHE PATH "Howdy log directory")
  # STRING, not PATH: CMake would turn a relative PATH value into an absolute one under
  # the build directory and the validation below would accept it.
  set(
   HOWDY_PRIVATE_LIBDIR
   ""
   CACHE STRING
   "Absolute directory holding a bundled OpenCV; becomes the RUNPATH of howdy and howdy-compare"
  )
  if(NOT HOWDY_PRIVATE_LIBDIR STREQUAL "")
    howdy_validate_private_libdir("${HOWDY_PRIVATE_LIBDIR}")
  endif()
  set(
   HOWDY_AUTH_HELPER_PATH
   ""
   CACHE FILEPATH
   "Path used by PAM to execute the privileged auth helper"
  )

  if(BUILD_TESTING AND HOWDY_ENABLE_PRIVILEGED_TESTS)
    if(HOWDY_PAM_DIR STREQUAL "")
      set(HOWDY_PAM_DIR "${CMAKE_INSTALL_PREFIX}/lib/security")
    endif()
    if(HOWDY_CONFIG_DIR STREQUAL "")
      set(HOWDY_CONFIG_DIR "${CMAKE_INSTALL_PREFIX}/etc/howdy")
    endif()
    if(HOWDY_MODELS_DIR STREQUAL "")
      set(HOWDY_MODELS_DIR "${CMAKE_INSTALL_PREFIX}/share/howdy/models")
    endif()
    if(HOWDY_USER_MODELS_DIR STREQUAL "")
      set(HOWDY_USER_MODELS_DIR "${CMAKE_INSTALL_PREFIX}/etc/howdy/models")
    endif()
    if(HOWDY_LOG_PATH STREQUAL "/var/log/howdy")
      set(HOWDY_LOG_PATH "${CMAKE_INSTALL_PREFIX}/var/log/howdy")
    endif()
  endif()

  if(HOWDY_PAM_DIR STREQUAL "")
    set(HOWDY_PAM_DIR "${CMAKE_INSTALL_FULL_LIBDIR}/security")
  endif()
  if(HOWDY_CONFIG_DIR STREQUAL "")
    set(HOWDY_CONFIG_DIR "${CMAKE_INSTALL_FULL_SYSCONFDIR}/howdy")
  endif()
  if(HOWDY_MODELS_DIR STREQUAL "")
    set(HOWDY_MODELS_DIR "${CMAKE_INSTALL_FULL_DATADIR}/howdy/models")
  endif()
  if(HOWDY_USER_MODELS_DIR STREQUAL "")
    set(HOWDY_USER_MODELS_DIR "${HOWDY_CONFIG_DIR}/models")
  endif()

  foreach(path IN ITEMS HOWDY_PAM_DIR HOWDY_CONFIG_DIR HOWDY_MODELS_DIR
                        HOWDY_USER_MODELS_DIR HOWDY_LOG_PATH)
    cmake_path(ABSOLUTE_PATH ${path} BASE_DIRECTORY "${CMAKE_INSTALL_PREFIX}" NORMALIZE)
  endforeach()

  set(HOWDY_CONFIG_PATH "${HOWDY_CONFIG_DIR}/config.ini")
  cmake_path(NORMAL_PATH HOWDY_CONFIG_PATH)
  set(
   HOWDY_PACKAGED_CONFIG_PATH
   "${PROJECT_BINARY_DIR}/generated/config/config.ini"
  )
  set(HOWDY_HELPER_DIR "${CMAKE_INSTALL_FULL_LIBEXECDIR}/howdy")
  set(HOWDY_COMPARE_PROCESS_PATH "${HOWDY_HELPER_DIR}/howdy-compare")
  if(HOWDY_AUTH_HELPER_PATH STREQUAL "")
    set(HOWDY_AUTH_HELPER_PATH "${HOWDY_HELPER_DIR}/howdy-auth-helper")
  endif()

  cmake_path(NORMAL_PATH HOWDY_HELPER_DIR)
  cmake_path(NORMAL_PATH HOWDY_COMPARE_PROCESS_PATH)
  cmake_path(ABSOLUTE_PATH HOWDY_AUTH_HELPER_PATH BASE_DIRECTORY "${CMAKE_INSTALL_PREFIX}" NORMALIZE)
endmacro()
