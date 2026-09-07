if(NOT DEFINED NM OR NM STREQUAL "")
	message(FATAL_ERROR "CMake did not provide an nm tool")
endif()
if(NOT DEFINED MODULE OR NOT EXISTS "${MODULE}")
	message(FATAL_ERROR "PAM module does not exist: ${MODULE}")
endif()

execute_process(
	COMMAND "${NM}" -D --defined-only --format=posix "${MODULE}"
	RESULT_VARIABLE nm_result
	OUTPUT_VARIABLE nm_output
	ERROR_VARIABLE nm_error
)
if(NOT nm_result EQUAL 0)
	message(FATAL_ERROR "Failed to inspect PAM module exports: ${nm_error}")
endif()

set(actual_exports)
string(REPLACE "\n" ";" nm_lines "${nm_output}")
foreach(line IN LISTS nm_lines)
	if(line MATCHES "^([^ \t]+)[ \t]")
		set(symbol "${CMAKE_MATCH_1}")
		string(REGEX REPLACE "@.*$" "" symbol "${symbol}")
		list(APPEND actual_exports "${symbol}")
	endif()
endforeach()
list(REMOVE_DUPLICATES actual_exports)
list(SORT actual_exports)

set(expected_exports
	pam_sm_acct_mgmt
	pam_sm_authenticate
	pam_sm_chauthtok
	pam_sm_close_session
	pam_sm_open_session
	pam_sm_setcred
)
list(SORT expected_exports)

if(NOT actual_exports STREQUAL expected_exports)
	string(JOIN "\n  " actual_text ${actual_exports})
	string(JOIN "\n  " expected_text ${expected_exports})
	message(FATAL_ERROR
		"Unexpected PAM module exports.\n"
		"Expected:\n  ${expected_text}\n"
		"Actual:\n  ${actual_text}"
	)
endif()
