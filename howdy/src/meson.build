if meson.is_subproject()
project('howdy', 'cpp', license: 'MIT', version: 'beta', meson_version: '>= 0.64.0')
endif


datadir = get_option('prefix') / get_option('datadir') / 'howdy'
py_conf = configuration_data(paths_dict)
py_conf.set('data_dir', datadir)

py = import('python').find_installation(paths_dict.get('python_path'))
py.dependency()

py_paths = configure_file(
    input: 'lib/paths.py.in',
    output: 'paths.py',
    configuration: py_conf,
)

native_cpp_conf = configuration_data(paths_dict)
native_cpp_conf.set('default_dev_config_path', meson.current_source_dir() / 'config' / 'config.ini')
native_cpp_conf.set('installed_config_path', config_path)

py_sources = [
    'lib/cli/__init__.py',
    'lib/cli/add.py',
    'lib/cli/clear.py',
    'lib/cli/config.py',
    'lib/cli/disable.py',
    'lib/cli/download_models.py',
    'lib/cli/list.py',
    'lib/cli/remove.py',
    'lib/cli/set.py',
    'lib/cli/snap.py',
    'lib/cli/test.py',
    'main.py',
    'lib/compare.py',
    'lib/config_utils.py',
    'lib/core/__init__.py',
    'lib/core/detector.py',
    'lib/i18n.py',
    'lib/paths_factory.py',
    'lib/recorders/__init__.py',
    'lib/recorders/ffmpeg_reader.py',
    'lib/recorders/video_capture.py',
    'lib/snapshot.py',
    py_paths,
]

# Include PAM module
if get_option('install_in_site_packages')
    pysourcesinstalldir = join_paths(py.get_install_dir(), 'howdy')
else
    pysourcesinstalldir = get_option('py_sources_dir') != '' ? get_option('py_sources_dir') / 'howdy' : join_paths(get_option('prefix'), get_option('libdir'), 'howdy')
endif

pam_module_conf_data = configuration_data(paths_dict)
pam_module_conf_data.set('compare_script_path', join_paths(pysourcesinstalldir, 'lib', 'compare.py'))
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

if get_option('install_in_site_packages')
    py.install_sources(
        py_sources,
        subdir: 'howdy',
        preserve_path: true,
        install_tag: 'py_sources',
    )
else
    install_data(
        py_sources,
        preserve_path: true,
        install_dir: pysourcesinstalldir,
        install_mode: 'r--r--r--',
        install_tag: 'py_sources',
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

cli_path = join_paths(pysourcesinstalldir, 'main.py')
conf_data = configuration_data({ 'script_path': cli_path, 'python_path': py.full_path() })

bin_name = 'howdy'
bin = configure_file(
    input: 'bin/howdy.in',
    output: bin_name,
    configuration: conf_data
)
install_data(
  bin,
  install_mode: 'rwxr-xr-x',
  install_dir: get_option('bindir'),
  install_tag: 'bin',
)
