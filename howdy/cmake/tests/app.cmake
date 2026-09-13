howdy_add_native_test(
	howdy_command_catalog_test
	native-command-catalog
	tests/app/command_catalog_test.cpp
)
target_link_libraries(howdy_command_catalog_test PRIVATE howdy_command_catalog)

howdy_add_native_test(
	howdy_man_reference_test
	native-man-reference
	tests/docs/man_reference_test.cpp
)
target_link_libraries(
	howdy_man_reference_test
	PRIVATE
		howdy_man_reference
		howdy_command_catalog
		howdy_pam_option_catalog
		howdy_config_schema
)

howdy_add_native_test(
	howdy_dispatch_test
	native-howdy-dispatch
	tests/app/howdy_dispatch_test.cpp
	tests/app/howdy_completion_test.cpp
)
target_link_libraries(howdy_dispatch_test PRIVATE howdy_cli_core)

howdy_add_native_test(
	howdy_main_test
	native-howdy-main
	tests/app/howdy_main_test.cpp
	src/bin/howdy_main.cpp
)
target_compile_definitions(
	howdy_main_test
	PRIVATE
		HOWDY_MAIN_ENTRYPOINT=HowdyMainTestEntry
		HOWDY_MAIN_DISPATCH=HowdyMainTestDispatch
)
