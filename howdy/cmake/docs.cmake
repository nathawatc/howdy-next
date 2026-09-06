add_library(howdy_man_reference STATIC src/docs/man_reference.cpp)
howdy_configure_native_target(howdy_man_reference)
target_link_libraries(
	howdy_man_reference
	PUBLIC howdy_command_catalog howdy_config_schema
)
target_include_directories(
	howdy_man_reference
	PUBLIC "${PROJECT_SOURCE_DIR}/pam/include"
)

add_executable(howdy_docs_generator src/tools/generate_docs.cpp)
howdy_configure_native_target(howdy_docs_generator)
target_include_directories(
	howdy_docs_generator
	PRIVATE "${PROJECT_SOURCE_DIR}/pam/include"
)
target_link_libraries(
	howdy_docs_generator
	PRIVATE
		howdy_command_catalog
		howdy_pam_option_catalog
		howdy_config_schema
		howdy_man_reference
)
if(CMAKE_CROSSCOMPILING)
	set_property(
		TARGET howdy_docs_generator
		PROPERTY CROSSCOMPILING_EMULATOR "${CMAKE_CROSSCOMPILING_EMULATOR}"
	)
endif()

set(HOWDY_DOC_REFERENCE_DIR "${PROJECT_BINARY_DIR}/generated/docs")
set(
	HOWDY_COMMAND_REFERENCE_PATH
	"${HOWDY_DOC_REFERENCE_DIR}/howdy-commands.roff"
)
set(HOWDY_OPTION_REFERENCE_PATH "${HOWDY_DOC_REFERENCE_DIR}/howdy-options.roff")
set(
	HOWDY_WORKAROUND_REFERENCE_PATH
	"${HOWDY_DOC_REFERENCE_DIR}/pam-workarounds.roff"
)
set(
	HOWDY_CONFIG_REFERENCE_PATH
	"${HOWDY_DOC_REFERENCE_DIR}/howdy-ini-options.roff"
)
set(HOWDY_MAN1_PATH "${CMAKE_CURRENT_BINARY_DIR}/howdy.1")
set(HOWDY_MAN5_PATH "${CMAKE_CURRENT_BINARY_DIR}/howdy.ini.5")
set(HOWDY_MAN8_PATH "${CMAKE_CURRENT_BINARY_DIR}/pam_howdy.8")
set(HOWDY_MAN_COMPOSE_SCRIPT "${PROJECT_SOURCE_DIR}/cmake/compose_man.cmake")

add_custom_command(
	OUTPUT
		"${HOWDY_COMMAND_REFERENCE_PATH}"
		"${HOWDY_OPTION_REFERENCE_PATH}"
		"${HOWDY_WORKAROUND_REFERENCE_PATH}"
		"${HOWDY_CONFIG_REFERENCE_PATH}"
		"${HOWDY_MAN1_PATH}"
		"${HOWDY_MAN5_PATH}"
		"${HOWDY_MAN8_PATH}"
	COMMAND
		howdy_docs_generator
		--output-dir "${HOWDY_DOC_REFERENCE_DIR}"
	COMMAND
		"${CMAKE_COMMAND}"
		"-DMAN1_TEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/howdy.1.in"
		"-DMAN1_OUTPUT_PATH=${HOWDY_MAN1_PATH}"
		"-DMAN5_TEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/howdy.ini.5.in"
		"-DMAN5_OUTPUT_PATH=${HOWDY_MAN5_PATH}"
		"-DMAN8_TEMPLATE=${PROJECT_SOURCE_DIR}/pam/pam_howdy.8.in"
		"-DMAN8_OUTPUT_PATH=${HOWDY_MAN8_PATH}"
		"-DCOMMAND_REFERENCE=${HOWDY_COMMAND_REFERENCE_PATH}"
		"-DOPTION_REFERENCE=${HOWDY_OPTION_REFERENCE_PATH}"
		"-DWORKAROUND_REFERENCE=${HOWDY_WORKAROUND_REFERENCE_PATH}"
		"-DCONFIG_REFERENCE=${HOWDY_CONFIG_REFERENCE_PATH}"
		"-DPROJECT_VERSION=${PROJECT_VERSION}"
		"-DHOWDY_MAN_DATE=${HOWDY_MAN_DATE}"
		"-DHOWDY_CONFIG_DIR=${HOWDY_CONFIG_DIR}"
		"-DHOWDY_CONFIG_PATH=${HOWDY_CONFIG_PATH}"
		"-DHOWDY_MODELS_DIR=${HOWDY_MODELS_DIR}"
		"-DHOWDY_USER_MODELS_DIR=${HOWDY_USER_MODELS_DIR}"
		"-DHOWDY_AUTH_HELPER_PATH=${HOWDY_AUTH_HELPER_PATH}"
		-P "${HOWDY_MAN_COMPOSE_SCRIPT}"
	DEPENDS
		howdy_docs_generator
		"${CMAKE_CURRENT_SOURCE_DIR}/howdy.1.in"
		"${CMAKE_CURRENT_SOURCE_DIR}/howdy.ini.5.in"
		"${PROJECT_SOURCE_DIR}/pam/pam_howdy.8.in"
		"${PROJECT_SOURCE_DIR}/howdy/include/app/command_catalog.hpp"
		"${PROJECT_SOURCE_DIR}/howdy/src/app/command_catalog.cpp"
		"${PROJECT_SOURCE_DIR}/howdy/include/config/config_schema.hpp"
		"${PROJECT_SOURCE_DIR}/howdy/src/config/config_schema.cpp"
		"${PROJECT_SOURCE_DIR}/howdy/include/docs/man_reference.hpp"
		"${PROJECT_SOURCE_DIR}/howdy/src/docs/man_reference.cpp"
		"${PROJECT_SOURCE_DIR}/howdy/src/tools/generate_docs.cpp"
		"${PROJECT_SOURCE_DIR}/pam/include/module/pam_option_catalog.hpp"
		"${PROJECT_SOURCE_DIR}/pam/src/module/pam_option_catalog.cpp"
		"${HOWDY_MAN_COMPOSE_SCRIPT}"
	COMMENT "Generating and composing Howdy manual pages"
	VERBATIM
)
add_custom_target(howdy_docs ALL DEPENDS "${HOWDY_MAN1_PATH}"
                  "${HOWDY_MAN5_PATH}" "${HOWDY_MAN8_PATH}")

install(
	FILES "${HOWDY_MAN1_PATH}"
	DESTINATION "${CMAKE_INSTALL_MANDIR}/man1"
)
install(
	FILES "${HOWDY_MAN5_PATH}"
	DESTINATION "${CMAKE_INSTALL_MANDIR}/man5"
)
install(
	FILES "${HOWDY_MAN8_PATH}"
	DESTINATION "${CMAKE_INSTALL_MANDIR}/man8"
)
