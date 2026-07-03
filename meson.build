project(
	'howdy',
	'cpp',
	license: 'MIT',
	version: '3.1.2',
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

prefix = get_option('prefix')
datadir = get_option('datadir')
sysconfdir = get_option('sysconfdir')
libexecdir = get_option('libexecdir')

modelsdir = get_option('models_dir')
if modelsdir == ''
	modelsdir = prefix / datadir / 'howdy' / 'models'
endif

confdir = get_option('config_dir')
if confdir == ''
	confdir = prefix / sysconfdir / 'howdy'
endif

usermodelsdir = get_option('user_models_dir')
if usermodelsdir == ''
	usermodelsdir = confdir / 'models'
endif

logpath = get_option('log_path')

config_path = confdir / 'config.ini'
helperdir = prefix / libexecdir / 'howdy'
compare_process_path = helperdir / 'howdy-compare'
auth_helper_path = helperdir / 'howdy-auth-helper'

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

subdir('howdy')
