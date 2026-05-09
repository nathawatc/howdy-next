project('howdy', 'cpp', license: 'MIT', version: '1.0.0', meson_version: '>= 1.1')

modelsdir = get_option('models_dir') != '' ? get_option('models_dir') : join_paths(get_option('prefix'), get_option('datadir'), 'howdy', 'models')
confdir = get_option('config_dir') != '' ? get_option('config_dir') : join_paths(get_option('prefix'), get_option('sysconfdir'), 'howdy')
usermodelsdir = get_option('user_models_dir') != '' ? get_option('user_models_dir') : join_paths(confdir, 'models')
logpath = get_option('log_path')
pythonpath = get_option('python_path')

config_path = join_paths(confdir, 'config.ini')

paths_dict = {
    'config_dir': confdir,
    'models_dir': modelsdir,
    'user_models_dir': usermodelsdir,
    'log_path': logpath,
    'python_path': pythonpath
}

subdir('howdy')
