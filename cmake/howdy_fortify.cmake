# Adds _FORTIFY_SOURCE=3 for optimized configurations unless the builder already
# chose a level in the C++ flags. Distribution build flags (dpkg-buildflags, rpm)
# define it themselves, and a second definition with another value is a redefinition
# warning that -Werror turns into an error. The builder's level wins; it is never
# lowered with -U.
function(howdy_add_fortify_source)
  string(FIND "${CMAKE_CXX_FLAGS}" "_FORTIFY_SOURCE" howdy_fortify_in_base_flags)
  if(NOT howdy_fortify_in_base_flags EQUAL -1)
    return()
  endif()

  foreach(howdy_fortify_config IN ITEMS Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${howdy_fortify_config}" howdy_fortify_config_upper)
    string(
     FIND "${CMAKE_CXX_FLAGS_${howdy_fortify_config_upper}}" "_FORTIFY_SOURCE"
     howdy_fortify_in_config_flags
    )
    if(howdy_fortify_in_config_flags EQUAL -1)
      add_compile_definitions(
       "$<$<CONFIG:${howdy_fortify_config}>:_FORTIFY_SOURCE=3>"
      )
    endif()
  endforeach()
endfunction()
