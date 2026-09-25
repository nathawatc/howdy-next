execute_process(
 COMMAND "${BINARY}" ${ARGUMENTS}
 RESULT_VARIABLE actual_status
 OUTPUT_VARIABLE actual_stdout
 ERROR_VARIABLE actual_stderr
)

if(NOT "${actual_status}" STREQUAL "${EXPECTED_STATUS}" OR
   NOT "${actual_stdout}" STREQUAL "${EXPECTED_STDOUT}" OR
   NOT "${actual_stderr}" STREQUAL "${EXPECTED_STDERR}")
  message(FATAL_ERROR
  "${BINARY}: unexpected result\n"
  "Expected status: ${EXPECTED_STATUS}\n"
  "Actual status: ${actual_status}\n"
  "Expected stdout: [${EXPECTED_STDOUT}]\n"
  "Actual stdout: [${actual_stdout}]\n"
  "Expected stderr: [${EXPECTED_STDERR}]\n"
  "Actual stderr: [${actual_stderr}]"
 )
endif()
