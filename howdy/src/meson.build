if meson.is_subproject()
project('howdy', 'cpp', license: 'MIT', version: 'beta', meson_version: '>= 0.64.0')
endif


datadir = get_option('prefix') / get_option('datadir') / 'howdy'

native_cpp_conf = configuration_data(paths_dict)
native_cpp_conf.set('default_dev_config_path', config_path)
native_cpp_conf.set('installed_config_path', config_path)

pam_module_conf_data = configuration_data(paths_dict)
pam_module_conf_data.set('compare_process_path', join_paths(get_option('prefix'), get_option('libdir'), 'howdy', 'howdy-compare'))
pam_module_conf_data.set('config_file_path', config_path)
subdir('pam')
subdir('lib/cpp')
if get_option('install_pam_config')
    # pamdir is inherited from the pam subproject
    pam_config = configure_file(
        input: 'pam-config/howdy.in',
        output: 'pam-config',
        configuration: {'pamdir': pamdir}
    )
    install_data(
        pam_config,
        install_dir: get_option('prefix') / get_option('datadir') / 'pam-configs',
        install_mode: 'rwxr-xr-x',
        install_tag: 'pam',
        rename: 'howdy',
    )
endif

autocomplete = configure_file(
    input: 'autocomplete/howdy.in',
    output: 'autocomplete',
    configuration: configuration_data({ 'config_path': config_path })
)
install_data(
    autocomplete,
    install_dir: join_paths(get_option('prefix'), get_option('datadir'), 'bash-completion', 'completions'),
    install_mode: 'rwxr--r--',
    install_tag: 'bash_completion',
    rename: 'howdy',
)

fs = import('fs')
if not fs.exists(config_path)
    install_data('config/config.ini', install_dir: confdir, install_mode: 'rwxr--r--', install_tag: 'config')
endif

install_emptydir(modelsdir, install_tag: 'models')

install_man('../howdy.1')
