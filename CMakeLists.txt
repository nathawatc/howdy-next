cmake_minimum_required(VERSION 3.31)

project(
	howdy
	VERSION 3.4.0
	DESCRIPTION "C++ facial-recognition authentication for Linux"
	HOMEPAGE_URL "https://codeberg.org/nathawat/howdy-next"
	LANGUAGES CXX
)

string(TIMESTAMP HOWDY_MAN_DATE "%Y-%m-%d" UTC)
if(HOWDY_MAN_DATE STREQUAL "")
	message(FATAL_ERROR "Cannot determine UTC documentation date")
endif()

if(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
	message(
		FATAL_ERROR
			"Cross-compiling Howdy requires CMAKE_CROSSCOMPILING_EMULATOR "
			"to run the packaged-config generator"
	)
endif()

if(NOT CMAKE_CONFIGURATION_TYPES AND CMAKE_BUILD_TYPE STREQUAL "")
	set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)
include(CTest)
include(CheckPIESupported)
include(CheckCXXSourceCompiles)
include(CMakePushCheckState)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(Git QUIET)

if(NOT DEFINED HOWDY_GIT_COMMIT)
	set(
		HOWDY_GIT_COMMIT
		""
		CACHE STRING
		"Source commit override for Howdy version output"
	)
endif()
if(Git_FOUND)
	set(HOWDY_GIT_EXECUTABLE "${GIT_EXECUTABLE}")
else()
	set(HOWDY_GIT_EXECUTABLE "")
endif()

check_pie_supported()

cmake_push_check_state(RESET)
set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
check_cxx_source_compiles(
	[=[
		#include <spawn.h>

		int main() {
			posix_spawn_file_actions_t actions;
			if (posix_spawn_file_actions_init(&actions) != 0) {
				return 1;
			}
			const int result = posix_spawn_file_actions_addclosefrom_np(&actions, 3);
			posix_spawn_file_actions_destroy(&actions);
			return result;
		}
	]=]
	HOWDY_HAVE_POSIX_SPAWN_FILE_ACTIONS_ADDCLOSEFROM_NP
)
cmake_pop_check_state()
if(NOT HOWDY_HAVE_POSIX_SPAWN_FILE_ACTIONS_ADDCLOSEFROM_NP)
	message(
		FATAL_ERROR
			"posix_spawn_file_actions_addclosefrom_np() is unavailable; "
			"GNU libc 2.34 or newer is required."
	)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT DEFINED CMAKE_UNITY_BUILD)
	set(CMAKE_UNITY_BUILD ON)
endif()
set(CMAKE_OPTIMIZE_DEPENDENCIES ON)

option(
	HOWDY_WARNINGS_AS_ERRORS
	"Treat compiler warnings as errors"
	OFF
)
option(
	HOWDY_USE_CCACHE
	"Use ccache to accelerate C++ recompilation when available"
	ON
)
option(
	HOWDY_ENABLE_PRIVILEGED_TESTS
	"Enable installed PAM/setuid end-to-end tests"
	OFF
)

if(HOWDY_USE_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
	find_program(HOWDY_CCACHE_PROGRAM ccache)
	if(HOWDY_CCACHE_PROGRAM)
		set(CMAKE_CXX_COMPILER_LAUNCHER "${HOWDY_CCACHE_PROGRAM}")
	endif()
endif()

if(
	BUILD_TESTING
	AND HOWDY_ENABLE_PRIVILEGED_TESTS
	AND NOT CMAKE_INSTALL_PREFIX
		MATCHES "^/opt/howdy-installed-pam-e2e-[A-Za-z0-9._-]+$"
)
	message(
		FATAL_ERROR
			"Privileged tests require an isolated CMAKE_INSTALL_PREFIX "
			"matching /opt/howdy-installed-pam-e2e-*"
	)
endif()

set(howdy_cxx_compiler "$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>")
add_compile_options(
	# Build performance.
	"$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-pipe>"

	# High-signal compiler diagnostics.
	"$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wall>"
	"$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wextra>"
	"$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wpedantic>"
	"$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wconversion>"
	"$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wsign-conversion>"
	"$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wshadow>"
	"$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wformat=2>"
	"$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wundef>"
	"$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wdate-time>"
	"$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wmissing-declarations>"
	"$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Woverloaded-virtual>"

	# Partial aggregate initialization is intentional. Remaining members are
	# value-initialized by C++, while clang-tidy checks unsafe member state.
	"$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wno-missing-field-initializers>"

	# Build hardening.
	"$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-fstack-protector-strong>"

	# Enable only in maintainer/CI builds.
	"$<$<AND:$<BOOL:${HOWDY_WARNINGS_AS_ERRORS}>,${howdy_cxx_compiler}>:-Werror>"
)

add_compile_definitions(
	GETTEXT_PACKAGE="howdy"

	"$<$<CONFIG:Release>:_FORTIFY_SOURCE=3>"
	"$<$<CONFIG:RelWithDebInfo>:_FORTIFY_SOURCE=3>"
	"$<$<CONFIG:MinSizeRel>:_FORTIFY_SOURCE=3>"
)

add_link_options(
	"LINKER:-z,relro"
	"LINKER:-z,now"
	"LINKER:--no-undefined"
	"LINKER:--as-needed"
)

pkg_check_modules(OPENCV5 REQUIRED IMPORTED_TARGET opencv5>=5.0.0)
pkg_check_modules(INIREADER REQUIRED IMPORTED_TARGET INIReader>=59)
pkg_check_modules(YYJSON REQUIRED IMPORTED_TARGET yyjson>=0.12.0)
pkg_check_modules(LIBCURL REQUIRED IMPORTED_TARGET libcurl>=7.85.0)
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
set(
	HOWDY_LICENSES_INSTALL_DIR
	"${CMAKE_INSTALL_DOCDIR}"
	CACHE STRING
	"Directory for Howdy license and third-party notice files"
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

set(HOWDY_CONFIG_PATH "${HOWDY_CONFIG_DIR}/config.ini")
set(
	HOWDY_PACKAGED_CONFIG_PATH
	"${PROJECT_BINARY_DIR}/generated/config/config.ini"
)
set(HOWDY_HELPER_DIR "${CMAKE_INSTALL_FULL_LIBEXECDIR}/howdy")
set(HOWDY_COMPARE_PROCESS_PATH "${HOWDY_HELPER_DIR}/howdy-compare")
set(HOWDY_AUTH_HELPER_PATH "${HOWDY_HELPER_DIR}/howdy-auth-helper")

if(BUILD_TESTING AND HOWDY_ENABLE_PRIVILEGED_TESTS)
	set(howdy_licenses_install_path "${HOWDY_LICENSES_INSTALL_DIR}")
	cmake_path(
		ABSOLUTE_PATH howdy_licenses_install_path
		BASE_DIRECTORY "${CMAKE_INSTALL_PREFIX}"
		NORMALIZE
	)
	foreach(
		isolated_path
		IN ITEMS
			"${CMAKE_INSTALL_FULL_BINDIR}"
			"${CMAKE_INSTALL_FULL_DATADIR}"
			"${CMAKE_INSTALL_FULL_LOCALEDIR}"
			"${CMAKE_INSTALL_FULL_MANDIR}"
			"${HOWDY_PAM_DIR}"
			"${HOWDY_CONFIG_DIR}"
			"${HOWDY_MODELS_DIR}"
			"${HOWDY_USER_MODELS_DIR}"
			"${HOWDY_LOG_PATH}"
			"${HOWDY_HELPER_DIR}"
			"${howdy_licenses_install_path}"
	)
		cmake_path(
			IS_PREFIX CMAKE_INSTALL_PREFIX "${isolated_path}"
			NORMALIZE path_is_isolated
		)
		if(NOT path_is_isolated)
			message(
				FATAL_ERROR
				"Privileged-test path escapes isolated prefix: ${isolated_path}"
			)
		endif()
	endforeach()
endif()
set(HOWDY_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${HOWDY_GENERATED_INCLUDE_DIR}")
set(
	HOWDY_GENERATED_VERSION_HEADER
	"${HOWDY_GENERATED_INCLUDE_DIR}/version.hpp"
)
set(HOWDY_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/howdy/include")
set(HOWDY_CMAKE_DIR "${PROJECT_SOURCE_DIR}/cmake")
set(
	HOWDY_VERSION_TEMPLATE
	"${HOWDY_INCLUDE_DIR}/howdy/version.hpp.in"
)
set(
	HOWDY_VERSION_GENERATOR
	"${HOWDY_CMAKE_DIR}/generate_version.cmake"
)
add_custom_target(
	howdy_version_metadata ALL
	COMMAND
		"${CMAKE_COMMAND}"
		"-DHOWDY_VERSION_TEMPLATE=${HOWDY_VERSION_TEMPLATE}"
		"-DHOWDY_VERSION_OUTPUT=${HOWDY_GENERATED_VERSION_HEADER}"
		"-DHOWDY_PROJECT_VERSION=${PROJECT_VERSION}"
		"-DHOWDY_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
		"-DHOWDY_GIT_EXECUTABLE=${HOWDY_GIT_EXECUTABLE}"
		"-DHOWDY_GIT_COMMIT=${HOWDY_GIT_COMMIT}"
		-P "${HOWDY_VERSION_GENERATOR}"
	BYPRODUCTS "${HOWDY_GENERATED_VERSION_HEADER}"
	DEPENDS
		"${HOWDY_VERSION_GENERATOR}"
		"${HOWDY_VERSION_TEMPLATE}"
	COMMENT "Generating Howdy version metadata"
	VERBATIM
)

file(
	WRITE "${PROJECT_BINARY_DIR}/cmake_install_prefix.txt"
	"${CMAKE_INSTALL_PREFIX}"
)
configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/cmake/install_prefix_guard.cmake.in"
	"${PROJECT_BINARY_DIR}/cmake_install_prefix_guard.cmake"
	@ONLY
)
install(SCRIPT "${PROJECT_BINARY_DIR}/cmake_install_prefix_guard.cmake")

install(
	FILES
		"${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
		"${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
		"${CMAKE_CURRENT_SOURCE_DIR}/third_party/licenses/YUNET-MIT.txt"
		"${CMAKE_CURRENT_SOURCE_DIR}/third_party/licenses/SFACE-APACHE-2.0.txt"
	DESTINATION "${HOWDY_LICENSES_INSTALL_DIR}"
)

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

function(howdy_add_compile_commands_mirror target)
	if(NOT CMAKE_EXPORT_COMPILE_COMMANDS)
		return()
	endif()

	get_target_property(target_sources ${target} SOURCES)
	if(NOT target_sources)
		return()
	endif()

	get_target_property(target_type ${target} TYPE)
	set(mirror_target "${target}__compile_commands")
	if(target_type STREQUAL "EXECUTABLE")
		add_executable(${mirror_target} ${target_sources})
	elseif(target_type STREQUAL "STATIC_LIBRARY")
		add_library(${mirror_target} STATIC ${target_sources})
	elseif(target_type STREQUAL "SHARED_LIBRARY")
		add_library(${mirror_target} SHARED ${target_sources})
	elseif(target_type STREQUAL "MODULE_LIBRARY")
		add_library(${mirror_target} MODULE ${target_sources})
	elseif(target_type STREQUAL "OBJECT_LIBRARY")
		add_library(${mirror_target} OBJECT ${target_sources})
	else()
		message(FATAL_ERROR
          "Unsupported compile-commands mirror target type: ${target_type}")
	endif()

	set_target_properties(${mirror_target} PROPERTIES EXCLUDE_FROM_ALL ON
                       UNITY_BUILD OFF EXPORT_COMPILE_COMMANDS ON
                       INCLUDE_DIRECTORIES
                       "$<TARGET_PROPERTY:${target},INCLUDE_DIRECTORIES>"
                       SYSTEM_INCLUDE_DIRECTORIES
                       "$<TARGET_PROPERTY:${target},SYSTEM_INCLUDE_DIRECTORIES>"
                       COMPILE_DEFINITIONS
                       "$<TARGET_PROPERTY:${target},COMPILE_DEFINITIONS>"
                       COMPILE_OPTIONS
                       "$<TARGET_PROPERTY:${target},COMPILE_OPTIONS>"
                       COMPILE_FEATURES
                       "$<TARGET_PROPERTY:${target},COMPILE_FEATURES>"
                       LINK_LIBRARIES
                       "$<TARGET_PROPERTY:${target},LINK_LIBRARIES>")

	if(target_type STREQUAL "SHARED_LIBRARY" OR target_type STREQUAL
    "MODULE_LIBRARY")
		get_target_property(target_define_symbol ${target} DEFINE_SYMBOL)
		if(target_define_symbol)
			set_target_properties(
				${mirror_target} PROPERTIES DEFINE_SYMBOL "${target_define_symbol}"
			)
		else()
			string(MAKE_C_IDENTIFIER "${target}" target_identifier)
			set_target_properties(
				${mirror_target} PROPERTIES DEFINE_SYMBOL "${target_identifier}_EXPORTS"
			)
		endif()
	endif()
endfunction()

function(howdy_defer_compile_commands_mirror target)
	if(NOT CMAKE_EXPORT_COMPILE_COMMANDS)
		return()
	endif()

	set_target_properties(${target} PROPERTIES EXPORT_COMPILE_COMMANDS OFF)
	cmake_language(
		EVAL CODE
			"cmake_language(DEFER CALL howdy_add_compile_commands_mirror [[${target}]])"
	)
endfunction()

function(howdy_configure_native_target target)
	howdy_defer_compile_commands_mirror(${target})
	add_dependencies(${target} howdy_version_metadata)
	target_include_directories(
		${target}
		PRIVATE
			"${HOWDY_GENERATED_INCLUDE_DIR}"
			"${PROJECT_SOURCE_DIR}/howdy/include"
	)
endfunction()

function(howdy_configure_pam_target target)
	howdy_defer_compile_commands_mirror(${target})
	add_dependencies(${target} howdy_version_metadata)
	target_include_directories(
		${target}
		PRIVATE
			"${HOWDY_GENERATED_INCLUDE_DIR}"
			"${PROJECT_SOURCE_DIR}/pam/include"
			"${PROJECT_SOURCE_DIR}/howdy/include"
	)
endfunction()

function(howdy_configure_native_test_target target)
	howdy_configure_native_target(${target})
	target_include_directories(
		${target}
		PRIVATE
			"${PROJECT_SOURCE_DIR}/howdy/tests/include"
			"${PROJECT_SOURCE_DIR}/tests/include"
	)
endfunction()

function(howdy_configure_pam_test_target target)
	howdy_configure_pam_target(${target})
	target_include_directories(
		${target}
		PRIVATE
			"${PROJECT_SOURCE_DIR}/pam/tests/include"
			"${PROJECT_SOURCE_DIR}/tests/include"
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

add_library(
	howdy_command_catalog
	STATIC
	howdy/src/app/command_catalog.cpp
	howdy/src/app/command_catalog/internal.hpp
	howdy/src/app/command_catalog/validation.cpp
)
howdy_configure_native_target(howdy_command_catalog)

add_library(
	howdy_pam_option_catalog
	STATIC
	pam/src/module/pam_option_catalog.cpp
)
howdy_configure_pam_target(howdy_pam_option_catalog)

add_subdirectory(howdy)
add_subdirectory(pam)
