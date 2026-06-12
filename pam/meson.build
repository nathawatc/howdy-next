inih_cpp = dependency('INIReader')
libevdev = dependency('libevdev')
libpam = meson.get_compiler('cpp').find_library('pam')
threads = dependency('threads')

# Translations
subdir('po')

# Paths
paths_h = configure_file(
	input: 'paths.hpp.in',
	output: 'paths.hpp',
	configuration: pam_module_conf_data
)

pamdir = get_option('pam_dir') != '' ? get_option('pam_dir') : join_paths(get_option('prefix'), get_option('libdir'), 'security')

shared_library(
	'pam_howdy',
	'../howdy/src/config/config_reader.cpp',
	'../howdy/src/config/runtime_config.cpp',
	'../howdy/src/config/config_schema.cpp',
	'../howdy/src/config/config_validation.cpp',
	'../howdy/src/storage/user_model_readiness.cpp',
	'src/auth_flow.cpp',
	'src/main.cpp',
	'src/enter_device.cpp',
	'src/native_prompt_conversation.cpp',
	'src/prompt_workaround.cpp',
	'src/status_mapping.cpp',
	'src/tty_restore.cpp',
	include_directories: include_directories('include', '../howdy/include'),
	cpp_args: ['-DHOWDY_RUNTIME_CONFIG_EXPLICIT_PATH_ONLY'],
	dependencies: [
		libpam,
		inih_cpp,
		threads,
		libevdev,
	],
	link_depends: [
		paths_h,
	],
	install: true,
	install_dir: pamdir,
	install_tag: 'pam_module',
	name_prefix: ''
)

pam_status_mapping_test = executable(
	'pam-status-mapping-test',
	'src/status_mapping.cpp',
	'src/tests/status_mapping_test.cpp',
	include_directories: include_directories('include', '../howdy/include'),
	dependencies: [
		libpam,
		inih_cpp,
		threads,
		libevdev,
	],
	install: false,
)

test(
	'pam-status-mapping',
	pam_status_mapping_test,
)

pam_prompt_workaround_test = executable(
	'pam-prompt-workaround-test',
	'src/prompt_workaround.cpp',
	'src/tests/prompt_workaround_test.cpp',
	include_directories: include_directories('include', '../howdy/include'),
	dependencies: [
		libpam,
		inih_cpp,
		threads,
		libevdev,
	],
	install: false,
)

test(
	'pam-prompt-workaround',
	pam_prompt_workaround_test,
)

pam_main_entrypoints_test = executable(
	'pam-main-entrypoints-test',
	'src/main.cpp',
	'src/tests/main_entrypoints_test.cpp',
	include_directories: include_directories('include', '../howdy/include'),
	dependencies: [
		libpam,
		inih_cpp,
		threads,
		libevdev,
	],
	install: false,
)

test(
	'pam-main-entrypoints',
	pam_main_entrypoints_test,
)

pam_native_prompt_conversation_test = executable(
	'pam-native-prompt-conversation-test',
	'src/native_prompt_conversation.cpp',
	'src/tests/native_prompt_conversation_test.cpp',
	include_directories: include_directories('include', '../howdy/include'),
	cpp_args: ['-DHOWDY_PAM_TESTING'],
	dependencies: [
		libpam,
		inih_cpp,
		threads,
		libevdev,
	],
	install: false,
)

test(
	'pam-native-prompt-conversation',
	pam_native_prompt_conversation_test,
)

pam_optional_task_test = executable(
	'pam-optional-task-test',
	'src/tests/optional_task_test.cpp',
	include_directories: include_directories('include', '../howdy/include'),
	dependencies: [
		threads,
	],
	install: false,
)

test(
	'pam-optional-task',
	pam_optional_task_test,
)

pam_tty_restore_test = executable(
	'pam-tty-restore-test',
	'src/tty_restore.cpp',
	'src/tests/tty_restore_test.cpp',
	include_directories: include_directories('include', '../howdy/include'),
	dependencies: [
		libpam,
	],
	install: false,
)

test(
	'pam-tty-restore',
	pam_tty_restore_test,
)

pam_auth_flow_helpers_test = executable(
	'pam-auth-flow-helpers-test',
	'../howdy/src/config/config_reader.cpp',
	'../howdy/src/config/runtime_config.cpp',
	'../howdy/src/config/config_schema.cpp',
	'../howdy/src/config/config_validation.cpp',
	'../howdy/src/storage/user_model_readiness.cpp',
	'src/auth_flow.cpp',
	'src/enter_device.cpp',
	'src/native_prompt_conversation.cpp',
	'src/prompt_workaround.cpp',
	'src/status_mapping.cpp',
	'src/tests/auth_flow_helpers_test.cpp',
	include_directories: include_directories('include', '../howdy/include'),
	cpp_args: ['-DHOWDY_PAM_TESTING', '-DHOWDY_RUNTIME_CONFIG_EXPLICIT_PATH_ONLY'],
	dependencies: [
		libpam,
		inih_cpp,
		threads,
		libevdev,
	],
	link_depends: [
		paths_h,
	],
	install: false,
)

test(
	'pam-auth-flow-helpers',
	pam_auth_flow_helpers_test,
)
