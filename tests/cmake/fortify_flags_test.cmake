cmake_minimum_required(VERSION 3.31)

foreach(required_variable IN ITEMS HOWDY_SOURCE_DIR HOWDY_TEST_ROOT HOWDY_CMAKE_GENERATOR)
  if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
    message(FATAL_ERROR "Missing fortify test variable: ${required_variable}")
  endif()
endforeach()

file(REMOVE_RECURSE "${HOWDY_TEST_ROOT}")

set(failures "")

# Configures the fixture with the given extra arguments and records which
# configurations received the project's _FORTIFY_SOURCE=3 definition.
function(run_case case_name)
  set(case_dir "${HOWDY_TEST_ROOT}/${case_name}")
  execute_process(
   COMMAND
    "${CMAKE_COMMAND}"
    -S "${HOWDY_SOURCE_DIR}/tests/cmake/fortify_flags_fixture"
    -B "${case_dir}"
    -G "${HOWDY_CMAKE_GENERATOR}"
    "-DHOWDY_SOURCE_DIR=${HOWDY_SOURCE_DIR}"
    ${ARGN}
   RESULT_VARIABLE configure_result
   OUTPUT_VARIABLE configure_output
   ERROR_VARIABLE configure_error
  )
  if(NOT configure_result EQUAL 0)
    message(
     FATAL_ERROR
     "Fixture configure failed for ${case_name}:\n${configure_output}\n${configure_error}"
    )
  endif()
  file(READ "${case_dir}/definitions.txt" definitions)
  foreach(config IN ITEMS Release RelWithDebInfo MinSizeRel Debug)
    string(FIND "${definitions}" "CONFIG:${config}>:_FORTIFY_SOURCE=3" position)
    if(position EQUAL -1)
      set(${case_name}_${config} OFF PARENT_SCOPE)
    else()
      set(${case_name}_${config} ON PARENT_SCOPE)
    endif()
  endforeach()
endfunction()

function(expect_config case_name config expected)
  set(actual "${${case_name}_${config}}")
  if(NOT actual STREQUAL expected)
    set(
     failures
     "${failures}\n${case_name}: ${config} has =3 '${actual}', expected ${expected}"
     PARENT_SCOPE
    )
  endif()
endfunction()

# No distribution flags: the project adds level 3 to every optimized configuration.
run_case(plain)
foreach(config IN ITEMS Release RelWithDebInfo MinSizeRel)
  expect_config(plain ${config} ON)
endforeach()
expect_config(plain Debug OFF)

# dpkg-buildflags style: the level is in the base C++ flags, so the project adds nothing.
run_case(dpkg "-DCMAKE_CXX_FLAGS=-O2 -D_FORTIFY_SOURCE=2")
foreach(config IN ITEMS Release RelWithDebInfo MinSizeRel Debug)
  expect_config(dpkg ${config} OFF)
endforeach()

# A level in one configuration's flags only suppresses that configuration.
run_case(per_config "-DCMAKE_CXX_FLAGS_RELEASE=-O3 -D_FORTIFY_SOURCE=2")
expect_config(per_config Release OFF)
expect_config(per_config RelWithDebInfo ON)
expect_config(per_config MinSizeRel ON)

# Unrelated flags that merely resemble the macro name do not suppress it.
run_case(unrelated "-DCMAKE_CXX_FLAGS=-O2 -Wno-fortify-source")
foreach(config IN ITEMS Release RelWithDebInfo MinSizeRel)
  expect_config(unrelated ${config} ON)
endforeach()

file(REMOVE_RECURSE "${HOWDY_TEST_ROOT}")

if(NOT failures STREQUAL "")
  message(FATAL_ERROR "Fortify flag checks failed:${failures}")
endif()
