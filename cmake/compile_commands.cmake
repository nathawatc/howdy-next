include_guard(GLOBAL)

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
		message(
			FATAL_ERROR
			"Unsupported compile-commands mirror target type: ${target_type}"
		)
	endif()

	set_target_properties(
		${mirror_target}
		PROPERTIES
			EXCLUDE_FROM_ALL ON
			UNITY_BUILD OFF
			EXPORT_COMPILE_COMMANDS ON
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
			"$<TARGET_PROPERTY:${target},LINK_LIBRARIES>"
	)

	if(
		target_type STREQUAL "SHARED_LIBRARY"
		OR target_type STREQUAL "MODULE_LIBRARY"
	)
		get_target_property(target_define_symbol ${target} DEFINE_SYMBOL)
		if(target_define_symbol)
			set_target_properties(
				${mirror_target}
				PROPERTIES DEFINE_SYMBOL "${target_define_symbol}"
			)
		else()
			string(MAKE_C_IDENTIFIER "${target}" target_identifier)
			set_target_properties(
				${mirror_target}
				PROPERTIES DEFINE_SYMBOL "${target_identifier}_EXPORTS"
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
