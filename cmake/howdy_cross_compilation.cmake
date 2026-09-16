function(howdy_require_crosscompiling_emulator)
	if(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
		message(
			FATAL_ERROR
				"Cross-compiling Howdy requires CMAKE_CROSSCOMPILING_EMULATOR "
				"to run the packaged-config generator"
		)
	endif()
endfunction()

function(howdy_set_crosscompiling_emulator target)
	if(CMAKE_CROSSCOMPILING)
		set_property(
			TARGET "${target}"
			PROPERTY CROSSCOMPILING_EMULATOR "${CMAKE_CROSSCOMPILING_EMULATOR}"
		)
	endif()
endfunction()
