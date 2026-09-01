cmake_minimum_required(VERSION 3.31)

if(
	NOT DEFINED MAN1_TEMPLATE
	OR NOT DEFINED MAN1_OUTPUT_PATH
	OR NOT DEFINED MAN5_TEMPLATE
	OR NOT DEFINED MAN5_OUTPUT_PATH
	OR NOT DEFINED MAN8_TEMPLATE
	OR NOT DEFINED MAN8_OUTPUT_PATH
	OR NOT DEFINED COMMAND_REFERENCE
	OR NOT DEFINED OPTION_REFERENCE
	OR NOT DEFINED WORKAROUND_REFERENCE
	OR NOT DEFINED CONFIG_REFERENCE
	OR NOT DEFINED PROJECT_VERSION
	OR NOT DEFINED HOWDY_MAN_DATE
	OR NOT DEFINED HOWDY_CONFIG_DIR
	OR NOT DEFINED HOWDY_CONFIG_PATH
	OR NOT DEFINED HOWDY_MODELS_DIR
	OR NOT DEFINED HOWDY_USER_MODELS_DIR
	OR NOT DEFINED HOWDY_AUTH_HELPER_PATH
)
	message(FATAL_ERROR "Man-page composition arguments are incomplete")
endif()

function(verify_regular_nonempty path description)
	if(
		NOT EXISTS "${path}"
		OR IS_DIRECTORY "${path}"
		OR IS_SYMLINK "${path}"
	)
		message(FATAL_ERROR "${description} is missing or unsafe: ${path}")
	endif()
	file(SIZE "${path}" file_size)
	if(file_size LESS 1)
		message(FATAL_ERROR "${description} is empty: ${path}")
	endif()
endfunction()

function(escape_roff_path value output_variable)
	string(FIND "${value}" "\n" newline_position)
	string(FIND "${value}" "\r" carriage_return_position)
	if(NOT newline_position EQUAL -1 OR NOT carriage_return_position EQUAL -1)
		message(FATAL_ERROR "Configured path contains a line break: ${value}")
	endif()
	set(escaped "${value}")
	string(REPLACE "\\" "\\e" escaped "${escaped}")
	string(REPLACE "-" "\\-" escaped "${escaped}")
	string(REPLACE "\"" "\\(dq" escaped "${escaped}")
	string(REPLACE "'" "\\(aq" escaped "${escaped}")
	if(value MATCHES "^[.']")
		set(escaped "\\&${escaped}")
	endif()
	set(${output_variable} "\"${escaped}\"" PARENT_SCOPE)
endfunction()

foreach(reference IN ITEMS COMMAND_REFERENCE OPTION_REFERENCE
        WORKAROUND_REFERENCE CONFIG_REFERENCE)
	verify_regular_nonempty("${${reference}}" "Generated man reference")
endforeach()

escape_roff_path("${HOWDY_CONFIG_DIR}" HOWDY_CONFIG_DIR_ROFF)
escape_roff_path("${HOWDY_CONFIG_PATH}" HOWDY_CONFIG_PATH_ROFF)
escape_roff_path("${HOWDY_MODELS_DIR}/" HOWDY_MODELS_DIR_ROFF)
escape_roff_path("${HOWDY_USER_MODELS_DIR}/" HOWDY_USER_MODELS_DIR_ROFF)
escape_roff_path("${HOWDY_AUTH_HELPER_PATH}" HOWDY_AUTH_HELPER_PATH_ROFF)

file(READ "${COMMAND_REFERENCE}" HOWDY_COMMAND_REFERENCE)
file(READ "${OPTION_REFERENCE}" HOWDY_OPTION_REFERENCE)
file(READ "${WORKAROUND_REFERENCE}" PAM_WORKAROUND_REFERENCE)
file(READ "${CONFIG_REFERENCE}" HOWDY_CONFIG_REFERENCE)

get_filename_component(MAN1_OUTPUT_DIR "${MAN1_OUTPUT_PATH}" DIRECTORY)
get_filename_component(MAN5_OUTPUT_DIR "${MAN5_OUTPUT_PATH}" DIRECTORY)
get_filename_component(MAN8_OUTPUT_DIR "${MAN8_OUTPUT_PATH}" DIRECTORY)
file(MAKE_DIRECTORY "${MAN1_OUTPUT_DIR}" "${MAN5_OUTPUT_DIR}"
     "${MAN8_OUTPUT_DIR}")

configure_file("${MAN1_TEMPLATE}" "${MAN1_OUTPUT_PATH}" @ONLY)
configure_file("${MAN5_TEMPLATE}" "${MAN5_OUTPUT_PATH}" @ONLY)
configure_file("${MAN8_TEMPLATE}" "${MAN8_OUTPUT_PATH}" @ONLY)
verify_regular_nonempty("${MAN1_OUTPUT_PATH}" "howdy(1)")
verify_regular_nonempty("${MAN5_OUTPUT_PATH}" "howdy.ini(5)")
verify_regular_nonempty("${MAN8_OUTPUT_PATH}" "pam_howdy(8)")
