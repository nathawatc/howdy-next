inih_cpp = dependency('INIReader', fallback: ['inih', 'INIReader_dep'])
libevdev = dependency('libevdev')
libpam = meson.get_compiler('cpp').find_library('pam')
threads = dependency('threads')

# Translations
subdir('po')

# Paths
paths_h = configure_file(
	input: 'paths.hh.in',
	output: 'paths.hh',
	configuration: pam_module_conf_data
)

pamdir = get_option('pam_dir') != '' ? get_option('pam_dir') : join_paths(get_option('prefix'), get_option('libdir'), 'security')

shared_library(
	'pam_howdy',
	'src/auth_flow.cc',
	'src/main.cc',
	'src/enter_device.cc',
	'src/prompt_workaround.cc',
	'src/status_mapping.cc',
	'src/tty_restore.cc',
	include_directories: include_directories('include', '../howdy/include'),
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
	'src/status_mapping.cc',
	'src/tests/status_mapping_test.cc',
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
	'src/prompt_workaround.cc',
	'src/tests/prompt_workaround_test.cc',
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
