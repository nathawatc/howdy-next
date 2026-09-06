add_executable(howdy_command_catalog_test tests/app/command_catalog_test.cpp)
howdy_configure_native_test_target(howdy_command_catalog_test)
target_link_libraries(howdy_command_catalog_test PRIVATE howdy_command_catalog)
add_test(NAME native-command-catalog COMMAND howdy_command_catalog_test)

add_executable(howdy_man_reference_test tests/docs/man_reference_test.cpp)
howdy_configure_native_test_target(howdy_man_reference_test)
target_link_libraries(
	howdy_man_reference_test
	PRIVATE
		howdy_man_reference
		howdy_command_catalog
		howdy_pam_option_catalog
		howdy_config_schema
)
add_test(NAME native-man-reference COMMAND howdy_man_reference_test)

add_executable(
	howdy_dispatch_test
	tests/app/howdy_dispatch_test.cpp
	tests/app/howdy_completion_test.cpp
)
howdy_configure_native_test_target(howdy_dispatch_test)
target_link_libraries(howdy_dispatch_test PRIVATE howdy_cli)
add_test(NAME native-howdy-dispatch COMMAND howdy_dispatch_test)

add_executable(
	howdy_main_test tests/app/howdy_main_test.cpp src/bin/howdy_main.cpp
)
howdy_configure_native_test_target(howdy_main_test)
target_compile_definitions(
	howdy_main_test
	PRIVATE
		HOWDY_MAIN_ENTRYPOINT=howdy_main_test_entry
		HOWDY_MAIN_DISPATCH=howdy_main_test_dispatch
)
add_test(NAME native-howdy-main COMMAND howdy_main_test)
