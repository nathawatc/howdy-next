project(
    'howdy',
    'cpp',
    license: 'MIT',
    version: '2.0.0',
    meson_version: '>= 1.11.0',
    default_options: [
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

paths_dict = {
    'config_dir': confdir,
    'models_dir': modelsdir,
    'user_models_dir': usermodelsdir,
    'log_path': logpath,
}

pam_module_conf_data = configuration_data(paths_dict)
pam_module_conf_data.set('compare_process_path', join_paths(get_option('prefix'), get_option('libdir'), 'howdy', 'howdy-compare'))
pam_module_conf_data.set('config_file_path', config_path)

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
