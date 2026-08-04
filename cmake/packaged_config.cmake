cmake_minimum_required(VERSION 3.31)

if(NOT DEFINED HOWDY_PACKAGED_CONFIG_ACTION
   OR NOT DEFINED HOWDY_PACKAGED_CONFIG_CANDIDATE
   OR NOT DEFINED HOWDY_PACKAGED_CONFIG_OUTPUT)
	message(FATAL_ERROR "Packaged-config script arguments are incomplete")
endif()

function(fail_packaged_config message_text)
	if(IS_DIRECTORY "${HOWDY_PACKAGED_CONFIG_CANDIDATE}")
		# Never recursively remove an unexpected candidate directory.
	elseif(EXISTS "${HOWDY_PACKAGED_CONFIG_CANDIDATE}"
		OR IS_SYMLINK "${HOWDY_PACKAGED_CONFIG_CANDIDATE}")
		file(REMOVE "${HOWDY_PACKAGED_CONFIG_CANDIDATE}")
	endif()
	message(FATAL_ERROR "${message_text}")
endfunction()

if(HOWDY_PACKAGED_CONFIG_ACTION STREQUAL "prepare")
	if(IS_DIRECTORY "${HOWDY_PACKAGED_CONFIG_CANDIDATE}")
		string(
			CONCAT error_message
			"Cannot remove build-owned packaged-config candidate directory: "
			"${HOWDY_PACKAGED_CONFIG_CANDIDATE}"
		)
		message(FATAL_ERROR "${error_message}")
	endif()
	if(EXISTS "${HOWDY_PACKAGED_CONFIG_CANDIDATE}"
		OR IS_SYMLINK "${HOWDY_PACKAGED_CONFIG_CANDIDATE}")
		file(REMOVE "${HOWDY_PACKAGED_CONFIG_CANDIDATE}")
	endif()
	if(EXISTS "${HOWDY_PACKAGED_CONFIG_CANDIDATE}"
		OR IS_SYMLINK "${HOWDY_PACKAGED_CONFIG_CANDIDATE}")
		string(
			CONCAT error_message
			"Cannot remove build-owned packaged-config candidate: "
			"${HOWDY_PACKAGED_CONFIG_CANDIDATE}"
		)
		message(FATAL_ERROR "${error_message}")
	endif()
elseif(HOWDY_PACKAGED_CONFIG_ACTION STREQUAL "publish")
	if(NOT EXISTS "${HOWDY_PACKAGED_CONFIG_CANDIDATE}"
		OR IS_DIRECTORY "${HOWDY_PACKAGED_CONFIG_CANDIDATE}"
		OR IS_SYMLINK "${HOWDY_PACKAGED_CONFIG_CANDIDATE}")
		string(
			CONCAT error_message
			"Packaged-config generator did not produce a regular candidate file: "
			"${HOWDY_PACKAGED_CONFIG_CANDIDATE}"
		)
		fail_packaged_config("${error_message}")
	endif()

	file(SIZE "${HOWDY_PACKAGED_CONFIG_CANDIDATE}" candidate_size)
	if(candidate_size LESS 1)
		string(
			CONCAT error_message
			"Packaged-config generator produced an empty candidate file: "
			"${HOWDY_PACKAGED_CONFIG_CANDIDATE}"
		)
		fail_packaged_config("${error_message}")
	endif()

	file(
		RENAME "${HOWDY_PACKAGED_CONFIG_CANDIDATE}"
		"${HOWDY_PACKAGED_CONFIG_OUTPUT}"
		RESULT rename_result
	)
	if(NOT rename_result STREQUAL "0")
		string(
			CONCAT error_message
			"Cannot atomically publish packaged config "
			"${HOWDY_PACKAGED_CONFIG_OUTPUT}: ${rename_result}"
		)
		fail_packaged_config("${error_message}")
	endif()
else()
	message(
		FATAL_ERROR
		"Unknown packaged-config action: ${HOWDY_PACKAGED_CONFIG_ACTION}"
	)
endif()
