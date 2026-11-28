set(HOWDY_FUZZER_SANITIZER_FLAGS "-fsanitize=fuzzer,address,undefined")

function(howdy_parse_fuzzing_mode raw_value build_testing out_mode)
  string(TOUPPER "${raw_value}" normalized_mode)
  if(
   NOT "${normalized_mode}" STREQUAL "AUTO"
   AND NOT "${normalized_mode}" STREQUAL "ON"
   AND NOT "${normalized_mode}" STREQUAL "OFF"
  )
    message(
     FATAL_ERROR
     "Invalid HOWDY_ENABLE_FUZZING value '${raw_value}': expected AUTO, ON, or OFF"
    )
  endif()
  if("${normalized_mode}" STREQUAL "ON" AND NOT build_testing)
    message(FATAL_ERROR "HOWDY_ENABLE_FUZZING=ON requires BUILD_TESTING=ON")
  endif()
  set(${out_mode} "${normalized_mode}" PARENT_SCOPE)
endfunction()
