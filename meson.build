project(
	'howdy',
	'cpp',
	license: 'MIT',
	version: '3.1.0',
	meson_version: '>= 1.11.0',
	default_options: [
		'cpp_std=c++23',
		'b_pie=true',
	],
)

cpp = meson.get_compiler('cpp')
if cpp.get_id() == 'gcc' or cpp.get_id() == 'clang'
	optimization_level = get_option('optimization')
	security_compile_args = ['-fstack-protector-strong']
	if optimization_level != '0' and optimization_level != 'plain'
		security_compile_args += ['-D_FORTIFY_SOURCE=3']
	endif
	add_project_arguments(security_compile_args, language: 'cpp')
	add_project_link_arguments(
		[
			'-Wl,-z,relro',
			'-Wl,-z,now',
		],
		language: 'cpp',
	)
endif

modelsdir = get_option('models_dir') != '' ? get_option('models_dir') : join_paths(get_option('prefix'), get_option('datadir'), 'howdy', 'models')
confdir = get_option('config_dir') != '' ? get_option('config_dir') : join_paths(get_option('prefix'), get_option('sysconfdir'), 'howdy')
usermodelsdir = get_option('user_models_dir') != '' ? get_option('user_models_dir') : join_paths(confdir, 'models')
logpath = get_option('log_path')

config_path = join_paths(confdir, 'config.ini')
compare_process_path = join_paths(get_option('prefix'), get_option('libdir'), 'howdy', 'howdy-compare')
auth_helper_path = join_paths(get_option('prefix'), get_option('libdir'), 'howdy', 'howdy-auth-helper')

paths_dict = {
	'auth_helper_path': auth_helper_path,
	'compare_process_path': compare_process_path,
	'default_dev_config_path': config_path,
	'installed_config_path': config_path,
	'config_dir': confdir,
	'models_dir': modelsdir,
	'user_models_dir': usermodelsdir,
	'log_path': logpath,
}

pam_module_conf_data = configuration_data(paths_dict)

subdir('pam')
if get_option('install_pam_config')
	pam_config = configure_file(
		input: 'pam/howdy.in',
		output: 'pam-config',
		configuration: {'pamdir': pamdir}
	)
	install_data(
		pam_config,
		install_dir: get_option('prefix') / get_option('datadir') / 'pam-configs',
		install_mode: 'rw-r--r--',
		install_tag: 'pam',
		rename: 'howdy',
	)
endif

subdir('howdy')
