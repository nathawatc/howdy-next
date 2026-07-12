cmake_minimum_required(VERSION 3.31)

project(
	howdy
	VERSION 3.3.1
	DESCRIPTION "C++ facial-recognition authentication for Linux"
	HOMEPAGE_URL "https://codeberg.org/nathawat/howdy-next"
	LANGUAGES CXX
)

if(NOT CMAKE_CONFIGURATION_TYPES AND CMAKE_BUILD_TYPE STREQUAL "")
	set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)
include(CTest)
include(CheckPIESupported)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

check_pie_supported()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -fstack-protector-strong)
add_compile_definitions(
	GETTEXT_PACKAGE="howdy"
	LOCALEDIR="${CMAKE_INSTALL_FULL_LOCALEDIR}"
	"$<$<CONFIG:Release>:_FORTIFY_SOURCE=3>"
	"$<$<CONFIG:RelWithDebInfo>:_FORTIFY_SOURCE=3>"
	"$<$<CONFIG:MinSizeRel>:_FORTIFY_SOURCE=3>"
)
add_link_options(
	-Wl,-z,relro -Wl,-z,now
	LINKER:--no-undefined LINKER:--as-needed
)

pkg_check_modules(OPENCV5 REQUIRED IMPORTED_TARGET opencv5>=5.0.0)
pkg_check_modules(INIREADER REQUIRED IMPORTED_TARGET INIReader)
pkg_check_modules(YYJSON REQUIRED IMPORTED_TARGET yyjson>=0.12.0)
pkg_check_modules(LIBCURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(OPENSSL REQUIRED IMPORTED_TARGET openssl)
pkg_check_modules(LIBEVDEV REQUIRED IMPORTED_TARGET libevdev)
pkg_check_modules(LIBACL REQUIRED IMPORTED_TARGET libacl)

find_path(PAM_INCLUDE_DIR NAMES security/pam_modules.h REQUIRED)
find_library(PAM_LIBRARY NAMES pam REQUIRED)
add_library(PAM::PAM UNKNOWN IMPORTED)
set_target_properties(
	PAM::PAM
	PROPERTIES
		IMPORTED_LOCATION "${PAM_LIBRARY}"
		INTERFACE_INCLUDE_DIRECTORIES "${PAM_INCLUDE_DIR}"
)

add_library(howdy_opencv INTERFACE)
target_include_directories(
	howdy_opencv SYSTEM INTERFACE ${OPENCV5_INCLUDE_DIRS}
)
target_link_libraries(howdy_opencv INTERFACE PkgConfig::OPENCV5)

set(HOWDY_PAM_DIR "" CACHE PATH "pam_howdy destination directory")
set(HOWDY_CONFIG_DIR "" CACHE PATH "Howdy configuration directory")
set(HOWDY_MODELS_DIR "" CACHE PATH "OpenCV ONNX models directory")
set(HOWDY_USER_MODELS_DIR "" CACHE PATH "User models directory")
set(HOWDY_LOG_PATH "/var/log/howdy" CACHE PATH "Howdy log directory")

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

set(HOWDY_CONFIG_PATH "${HOWDY_CONFIG_DIR}/config.ini")
set(HOWDY_HELPER_DIR "${CMAKE_INSTALL_FULL_LIBEXECDIR}/howdy")
set(HOWDY_COMPARE_PROCESS_PATH "${HOWDY_HELPER_DIR}/howdy-compare")
set(HOWDY_AUTH_HELPER_PATH "${HOWDY_HELPER_DIR}/howdy-auth-helper")
set(HOWDY_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${HOWDY_GENERATED_INCLUDE_DIR}")

file(WRITE "${PROJECT_BINARY_DIR}/cmake_install_prefix.txt" "${CMAKE_INSTALL_PREFIX}")
configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/cmake/install_prefix_guard.cmake.in"
	"${PROJECT_BINARY_DIR}/cmake_install_prefix_guard.cmake"
	@ONLY
)
install(SCRIPT "${PROJECT_BINARY_DIR}/cmake_install_prefix_guard.cmake")

set(default_dev_config_path "${HOWDY_CONFIG_PATH}")
set(installed_config_path "${HOWDY_CONFIG_PATH}")
set(models_dir "${HOWDY_MODELS_DIR}")
set(user_models_dir "${HOWDY_USER_MODELS_DIR}")
set(log_path "${HOWDY_LOG_PATH}")
set(compare_process_path "${HOWDY_COMPARE_PROCESS_PATH}")
set(auth_helper_path "${HOWDY_AUTH_HELPER_PATH}")
configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/howdy/include/config/paths.hpp.in"
	"${HOWDY_GENERATED_INCLUDE_DIR}/paths.hpp"
	@ONLY
)

function(howdy_configure_native_target target)
	target_include_directories(
		${target}
		PRIVATE
			"${HOWDY_GENERATED_INCLUDE_DIR}"
			"${PROJECT_SOURCE_DIR}/howdy/src"
			"${PROJECT_SOURCE_DIR}/howdy/include"
	)
endfunction()

function(howdy_configure_pam_target target)
	target_include_directories(
		${target}
		PRIVATE
			"${HOWDY_GENERATED_INCLUDE_DIR}"
			"${PROJECT_SOURCE_DIR}/pam/include"
			"${PROJECT_SOURCE_DIR}/howdy/include"
	)
endfunction()

function(howdy_expect_test_failure test_name)
	set_tests_properties(${test_name} PROPERTIES WILL_FAIL TRUE)
endfunction()

function(howdy_set_test_timeouts)
	get_property(HOWDY_DIRECTORY_TESTS DIRECTORY PROPERTY TESTS)
	if(HOWDY_DIRECTORY_TESTS)
		set_tests_properties(${HOWDY_DIRECTORY_TESTS} PROPERTIES TIMEOUT 30)
	endif()
endfunction()

add_subdirectory(pam)
add_subdirectory(howdy)
