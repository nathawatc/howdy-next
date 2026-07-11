find_program(GETTEXT_XGETTEXT_EXECUTABLE NAMES xgettext)
find_program(GETTEXT_MSGMERGE_EXECUTABLE NAMES msgmerge)
find_program(GETTEXT_MSGFMT_EXECUTABLE NAMES msgfmt)
if(NOT GETTEXT_XGETTEXT_EXECUTABLE OR NOT GETTEXT_MSGMERGE_EXECUTABLE OR NOT GETTEXT_MSGFMT_EXECUTABLE)
	message(FATAL_ERROR "Translation support requires xgettext, msgmerge, and msgfmt from gettext")
endif()

set(HOWDY_POT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/howdy.pot")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/POTFILES" HOWDY_POTFILES)
set(HOWDY_POT_SOURCES)
foreach(potfile IN LISTS HOWDY_POTFILES)
	string(STRIP "${potfile}" potfile)
	if(potfile STREQUAL "" OR potfile MATCHES "^#")
		continue()
	endif()
	set(pot_source "${CMAKE_CURRENT_SOURCE_DIR}/../${potfile}")
	if(NOT EXISTS "${pot_source}")
		message(FATAL_ERROR "Translation source '${potfile}' listed in POTFILES does not exist")
	endif()
	list(APPEND HOWDY_POT_SOURCES "${pot_source}")
endforeach()
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/LINGUAS" HOWDY_LINGUAS)
string(REGEX REPLACE "#[^\n]*" "" HOWDY_LINGUAS "${HOWDY_LINGUAS}")
string(REGEX REPLACE "[ \t\r\n]+" ";" HOWDY_LANGUAGES "${HOWDY_LINGUAS}")
list(FILTER HOWDY_LANGUAGES EXCLUDE REGEX "^$")

set(HOWDY_TRANSLATION_OUTPUTS)
set(HOWDY_TRANSLATION_PO_FILES)

foreach(language IN LISTS HOWDY_LANGUAGES)
	set(po_file "${CMAKE_CURRENT_SOURCE_DIR}/${language}.po")
	if(NOT EXISTS "${po_file}")
		message(FATAL_ERROR "Translation locale '${language}' listed in LINGUAS has no ${language}.po")
	endif()
	list(APPEND HOWDY_TRANSLATION_PO_FILES "${po_file}")
	set(mo_file "${CMAKE_CURRENT_BINARY_DIR}/${language}/LC_MESSAGES/howdy.mo")
	add_custom_command(
		OUTPUT "${mo_file}"
		COMMAND
			"${CMAKE_COMMAND}" -E make_directory
			"${CMAKE_CURRENT_BINARY_DIR}/${language}/LC_MESSAGES"
		COMMAND
			"${GETTEXT_MSGFMT_EXECUTABLE}" --check "--output-file=${mo_file}"
			"${po_file}"
		DEPENDS "${po_file}"
		CODEGEN
		VERBATIM
	)
	list(APPEND HOWDY_TRANSLATION_OUTPUTS "${mo_file}")
	install(
		FILES "${mo_file}"
		DESTINATION "${CMAKE_INSTALL_LOCALEDIR}/${language}/LC_MESSAGES"
	)
endforeach()

add_custom_target(
	howdy-pot
	COMMAND
		"${GETTEXT_XGETTEXT_EXECUTABLE}"
		"--directory=${CMAKE_CURRENT_SOURCE_DIR}/.."
		"--files-from=${CMAKE_CURRENT_SOURCE_DIR}/POTFILES"
		--keyword=S:1
		"--output=${HOWDY_POT_FILE}"
	DEPENDS
		"${CMAKE_CURRENT_SOURCE_DIR}/POTFILES"
		${HOWDY_POT_SOURCES}
	WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
	VERBATIM
)

set(HOWDY_MSGMERGE_COMMANDS)
foreach(po_file IN LISTS HOWDY_TRANSLATION_PO_FILES)
	list(APPEND
		HOWDY_MSGMERGE_COMMANDS
		COMMAND
		"${GETTEXT_MSGMERGE_EXECUTABLE}"
		--update
		--backup=none
		"${po_file}"
		"${HOWDY_POT_FILE}"
	)
endforeach()
add_custom_target(howdy-update-po DEPENDS howdy-pot ${HOWDY_MSGMERGE_COMMANDS})

add_custom_target(howdy-gmo ALL DEPENDS ${HOWDY_TRANSLATION_OUTPUTS})
