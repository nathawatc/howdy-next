set(
	required_inputs
	PAM_SOURCE_DIR
	POTFILES
	LINGUAS
	PO_DIR
	XGETTEXT
	MSGCMP
	MSGATTRIB
	WORK_DIR
)
foreach(required IN LISTS required_inputs)
	if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
		message(FATAL_ERROR "Missing required translation test input: ${required}")
	endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

file(STRINGS "${POTFILES}" potfile_lines)
set(listed_sources)
foreach(source IN LISTS potfile_lines)
	string(STRIP "${source}" source)
	if(source STREQUAL "" OR source MATCHES "^#")
		continue()
	endif()
	list(APPEND listed_sources "${source}")
endforeach()
list(SORT listed_sources)

set(listed_sources_unique ${listed_sources})
list(REMOVE_DUPLICATES listed_sources_unique)
if(NOT listed_sources STREQUAL listed_sources_unique)
	message(FATAL_ERROR "POTFILES contains duplicate translation sources")
endif()

file(
	GLOB_RECURSE translation_candidates
	RELATIVE "${PAM_SOURCE_DIR}"
	"${PAM_SOURCE_DIR}/include/*.hpp"
	"${PAM_SOURCE_DIR}/src/*.cpp"
	"${PAM_SOURCE_DIR}/src/*.hpp"
)
list(SORT translation_candidates)

set(discovered_sources)
foreach(source IN LISTS translation_candidates)
	string(MD5 source_id "${source}")
	set(probe "${WORK_DIR}/${source_id}.pot")
	execute_process(
		COMMAND
			"${XGETTEXT}"
			"--directory=${PAM_SOURCE_DIR}"
			--language=C++
			--keyword
			--keyword=Translate:1
			--omit-header
			--no-location
			"--output=${probe}"
			"${source}"
		RESULT_VARIABLE extract_result
		ERROR_VARIABLE extract_error
	)
	if(NOT extract_result EQUAL 0)
		message(
			FATAL_ERROR
			"xgettext failed for ${source}: ${extract_error}"
		)
	endif()
	if(EXISTS "${probe}")
		file(SIZE "${probe}" probe_size)
		if(probe_size GREATER 0)
			list(APPEND discovered_sources "${source}")
		endif()
	endif()
endforeach()
list(SORT discovered_sources)

if(NOT listed_sources STREQUAL discovered_sources)
	string(JOIN "\n  " listed_text ${listed_sources})
	string(JOIN "\n  " discovered_text ${discovered_sources})
	message(
		FATAL_ERROR
		"POTFILES does not match sources with extractable Translate() messages.\n"
		"POTFILES:\n  ${listed_text}\n"
		"Discovered:\n  ${discovered_text}"
	)
endif()

set(current_pot "${WORK_DIR}/current.pot")
execute_process(
	COMMAND
		"${XGETTEXT}"
		"--directory=${PAM_SOURCE_DIR}"
		"--files-from=${POTFILES}"
		--keyword
		--keyword=Translate:1
		--omit-header
		--no-location
		"--output=${current_pot}"
	RESULT_VARIABLE pot_result
	ERROR_VARIABLE pot_error
)
if(NOT pot_result EQUAL 0)
	message(FATAL_ERROR "Failed to extract current translations: ${pot_error}")
endif()

file(READ "${LINGUAS}" languages_text)
string(REGEX REPLACE "#[^\n]*" "" languages_text "${languages_text}")
string(REGEX REPLACE "[ \t\r\n]+" ";" languages "${languages_text}")
list(FILTER languages EXCLUDE REGEX "^$")

function(extract_active_msgids input_file output_variable)
	set(canonical_file "${WORK_DIR}/canonical-${output_variable}.properties")
	execute_process(
		COMMAND "${MSGATTRIB}" --no-obsolete --set-fuzzy "${input_file}"
		COMMAND
			"${MSGATTRIB}"
			--clear-fuzzy
			--empty
			--no-location
			--sort-output
			--properties-output
			"--output-file=${canonical_file}"
			-
		RESULT_VARIABLE canonical_result
		ERROR_VARIABLE canonical_error
	)
	if(NOT canonical_result EQUAL 0)
		message(
			FATAL_ERROR
			"Failed to canonicalize ${input_file}: ${canonical_error}"
		)
	endif()
	file(STRINGS "${canonical_file}" active_msgids)
	list(FILTER active_msgids INCLUDE REGEX "^!")
	list(REMOVE_ITEM active_msgids "!=")
	set(${output_variable} "${active_msgids}" PARENT_SCOPE)
endfunction()

extract_active_msgids("${current_pot}" source_msgids)

foreach(language IN LISTS languages)
	set(po_file "${PO_DIR}/${language}.po")
	if(NOT EXISTS "${po_file}")
		message(FATAL_ERROR "Missing translation catalog: ${po_file}")
	endif()
	execute_process(
		COMMAND
			"${MSGCMP}"
			--no-fuzzy-matching
			--use-fuzzy
			--use-untranslated
			"${po_file}"
			"${current_pot}"
		RESULT_VARIABLE compare_result
		OUTPUT_VARIABLE compare_output
		ERROR_VARIABLE compare_error
	)
	if(NOT compare_result EQUAL 0)
		message(
			FATAL_ERROR
			"Translation catalog ${language}.po is stale.\n"
			"${compare_output}${compare_error}"
		)
	endif()

	extract_active_msgids("${po_file}" catalog_msgids)
	if(NOT catalog_msgids STREQUAL source_msgids)
		string(JOIN "\n  " source_text ${source_msgids})
		string(JOIN "\n  " catalog_text ${catalog_msgids})
		message(
			FATAL_ERROR
			"Translation catalog ${language}.po has stale active messages.\n"
			"Source msgids:\n  ${source_text}\n"
			"Catalog msgids:\n  ${catalog_text}"
		)
	endif()
endforeach()
