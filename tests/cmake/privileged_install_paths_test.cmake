cmake_minimum_required(VERSION 3.31)

set(howdy_source_dir "${CMAKE_CURRENT_LIST_DIR}/../..")
include("${howdy_source_dir}/cmake/howdy_privileged_test_paths.cmake")

set(howdy_prefix "/opt/howdy-installed-pam-e2e-configure-test")
set(howdy_bindir "${howdy_prefix}/bin")
set(howdy_datadir "${howdy_prefix}/share")
set(howdy_localedir "${howdy_prefix}/share/locale")
set(howdy_mandir "${howdy_prefix}/share/man")
set(howdy_pam_dir "${howdy_prefix}/lib/security")
set(howdy_config_dir "${howdy_prefix}/etc/howdy")
set(howdy_models_dir "${howdy_prefix}/share/howdy/models")
set(howdy_user_models_dir "${howdy_prefix}/etc/howdy/models")
set(howdy_log_path "${howdy_prefix}/var/log/howdy")
set(howdy_helper_dir "${howdy_prefix}/libexec/howdy")
set(howdy_auth_helper_path "${howdy_helper_dir}/howdy-auth-helper")
set(howdy_licenses_install_dir "share/licenses/howdy")

if(DEFINED HOWDY_PRIVILEGED_TEST_CASE)
	if(HOWDY_PRIVILEGED_TEST_CASE STREQUAL "bindir")
		set(howdy_bindir "/usr/bin")
	elseif(HOWDY_PRIVILEGED_TEST_CASE STREQUAL "datadir")
		set(howdy_datadir "/usr/share")
	elseif(HOWDY_PRIVILEGED_TEST_CASE STREQUAL "localedir")
		set(howdy_localedir "/usr/share/locale")
	elseif(HOWDY_PRIVILEGED_TEST_CASE STREQUAL "mandir")
		set(howdy_mandir "/usr/share/man")
	elseif(HOWDY_PRIVILEGED_TEST_CASE STREQUAL "licenses")
		set(howdy_licenses_install_dir "/usr/share/licenses/howdy")
	endif()

	howdy_validate_privileged_test_paths(
		CMAKE_INSTALL_PREFIX "${howdy_prefix}"
		CMAKE_INSTALL_BINDIR "${howdy_bindir}"
		CMAKE_INSTALL_DATADIR "${howdy_datadir}"
		CMAKE_INSTALL_LOCALEDIR "${howdy_localedir}"
		CMAKE_INSTALL_MANDIR "${howdy_mandir}"
		HOWDY_PAM_DIR "${howdy_pam_dir}"
		HOWDY_CONFIG_DIR "${howdy_config_dir}"
		HOWDY_MODELS_DIR "${howdy_models_dir}"
		HOWDY_USER_MODELS_DIR "${howdy_user_models_dir}"
		HOWDY_LOG_PATH "${howdy_log_path}"
		HOWDY_HELPER_DIR "${howdy_helper_dir}"
		HOWDY_AUTH_HELPER_PATH "${howdy_auth_helper_path}"
		HOWDY_LICENSES_INSTALL_DIR "${howdy_licenses_install_dir}"
	)
else()
	set(rejected_cases
		bindir
		datadir
		localedir
		mandir
		licenses
	)
	set(rejected_paths
		/usr/bin
		/usr/share
		/usr/share/locale
		/usr/share/man
		/usr/share/licenses/howdy
	)
	list(LENGTH rejected_cases rejected_case_count)
	math(EXPR rejected_last_index "${rejected_case_count} - 1")
	foreach(rejected_index RANGE "${rejected_last_index}")
		list(GET rejected_cases "${rejected_index}" rejected_case)
		list(GET rejected_paths "${rejected_index}" rejected_path)
		execute_process(
			COMMAND
				"${CMAKE_COMMAND}"
				"-DHOWDY_PRIVILEGED_TEST_CASE=${rejected_case}"
				-P "${CMAKE_CURRENT_LIST_FILE}"
			RESULT_VARIABLE rejected_result
			OUTPUT_VARIABLE rejected_output
			ERROR_VARIABLE rejected_error
		)
		if(rejected_result EQUAL 0)
			message(
				FATAL_ERROR
				"Escaped install destination unexpectedly passed: "
				"${rejected_case}=${rejected_path}"
			)
		endif()
		string(
			FIND
			"${rejected_output}\n${rejected_error}"
			"Privileged-test path escapes isolated prefix: ${rejected_path}"
			rejected_message
		)
		if(rejected_message EQUAL -1)
			message(
				FATAL_ERROR
				"Escaped install destination omitted isolation diagnostic: "
				"${rejected_case}=${rejected_path}\n${rejected_output}\n${rejected_error}"
			)
		endif()
	endforeach()

	execute_process(
		COMMAND
			"${CMAKE_COMMAND}"
			-DHOWDY_PRIVILEGED_TEST_CASE=valid
			-P "${CMAKE_CURRENT_LIST_FILE}"
		RESULT_VARIABLE accepted_result
		OUTPUT_VARIABLE accepted_output
		ERROR_VARIABLE accepted_error
	)
	if(NOT accepted_result EQUAL 0)
		message(
			FATAL_ERROR
			"Valid isolated install destinations failed:\n"
			"${accepted_output}\n${accepted_error}"
		)
	endif()
endif()
