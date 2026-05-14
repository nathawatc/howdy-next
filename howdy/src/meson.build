opencv4 = dependency('opencv4')
inih_cpp = dependency('INIReader', fallback: ['inih', 'INIReader_dep'])
nlohmann_json = dependency('nlohmann_json')
libcurl = dependency('libcurl')
openssl = dependency('openssl')
datadir = get_option('prefix') / get_option('datadir') / 'howdy'
native_cpp_conf = configuration_data(paths_dict)
native_cpp_conf.set('default_dev_config_path', config_path)
native_cpp_conf.set('installed_config_path', config_path)

native_paths_h = configure_file(
    input: '../include/config/paths.hpp.in',
    output: 'paths.hpp',
    configuration: native_cpp_conf,
)

native_cpp_inc = include_directories('.', '../include')

native_cpp_sources = [
    'config/config_reader.cpp',
    'config/config_utils.cpp',
    'config/runtime_paths.cpp',
    'core/face_model.cpp',
    'recorders/video_capture.cpp',
    'storage/user_models.cpp',
]

native_cpp_lib = static_library(
    'howdy_native_cpp',
    native_cpp_sources,
    include_directories: native_cpp_inc,
    dependencies: [
        inih_cpp,
        libcurl,
        nlohmann_json,
        opencv4,
    ],
    install: false,
)

native_capture_smoke = executable(
    'howdy-capture-smoke',
    'smoke_capture.cpp',
    include_directories: native_cpp_inc,
    link_with: native_cpp_lib,
    dependencies: [
        inih_cpp,
        libcurl,
        nlohmann_json,
        opencv4,
    ],
    install: false,
)

test(
    'native-capture-smoke-help',
    native_capture_smoke,
    args: ['--help'],
)

native_compare = executable(
    'howdy-compare',
    'compare_args.cpp',
    'compare_logic.cpp',
    'compare.cpp',
    include_directories: native_cpp_inc,
    link_with: native_cpp_lib,
    dependencies: [
        inih_cpp,
        libcurl,
        nlohmann_json,
        opencv4,
    ],
    install: true,
    install_dir: join_paths(get_option('prefix'), get_option('libdir'), 'howdy'),
    install_tag: 'native_compare',
)

test(
    'native-compare-help',
    native_compare,
    args: ['--help'],
)

test(
    'native-compare-unknown-arg',
    native_compare,
    args: ['--unknown'],
    should_fail: true,
)

test(
    'native-compare-missing-user',
    native_compare,
    should_fail: true,
)

test(
    'native-compare-no-model',
    native_compare,
    args: ['coverage-user'],
    env: ['HOWDY_USER_MODELS_DIR=' + meson.current_build_dir() / 'compare-test-models-empty'],
    should_fail: true,
)

native_compare_args_test = executable(
    'howdy-compare-args-test',
    'compare_args.cpp',
    'tests/compare_args_test.cpp',
    include_directories: native_cpp_inc,
    dependencies: [
        inih_cpp,
        libcurl,
        nlohmann_json,
        opencv4,
    ],
    install: false,
)

test(
    'native-compare-args',
    native_compare_args_test,
)

native_compare_logic_test = executable(
    'howdy-compare-logic-test',
    'compare_logic.cpp',
    'tests/compare_logic_test.cpp',
    include_directories: native_cpp_inc,
    dependencies: [
        inih_cpp,
        libcurl,
        nlohmann_json,
        opencv4,
    ],
    install: false,
)

test(
    'native-compare-logic',
    native_compare_logic_test,
)

native_config_utils_test = executable(
    'howdy-config-utils-test',
    'tests/config_utils_test.cpp',
    include_directories: native_cpp_inc,
    link_with: native_cpp_lib,
    dependencies: [
        inih_cpp,
        libcurl,
        nlohmann_json,
        opencv4,
    ],
    install: false,
)

test(
    'native-config-utils',
    native_config_utils_test,
)

native_user_models_test = executable(
    'howdy-user-models-test',
    'tests/user_models_test.cpp',
    include_directories: native_cpp_inc,
    link_with: native_cpp_lib,
    dependencies: [
        inih_cpp,
        libcurl,
        nlohmann_json,
        opencv4,
    ],
    install: false,
)

test(
    'native-user-models',
    native_user_models_test,
)

native_runtime_paths_test = executable(
    'howdy-runtime-paths-test',
    'tests/runtime_paths_test.cpp',
    include_directories: native_cpp_inc,
    link_with: native_cpp_lib,
    dependencies: [
        inih_cpp,
        libcurl,
        nlohmann_json,
        opencv4,
    ],
    install: false,
)

test(
    'native-runtime-paths',
    native_runtime_paths_test,
)

native_config_reader_test = executable(
    'howdy-config-reader-test',
    'tests/config_reader_test.cpp',
    include_directories: native_cpp_inc,
    link_with: native_cpp_lib,
    dependencies: [
        inih_cpp,
        libcurl,
        nlohmann_json,
        opencv4,
    ],
    install: false,
)

test(
    'native-config-reader',
    native_config_reader_test,
)

subdir('cli')

native_add = executable(
    'howdy-add',
    'cli/add.cpp',
    'cli/add_main.cpp',
    include_directories: native_cpp_inc,
    link_with: native_cpp_lib,
    dependencies: [
        inih_cpp,
        nlohmann_json,
        opencv4,
    ],
    install: true,
    install_dir: join_paths(get_option('prefix'), get_option('libdir'), 'howdy'),
    install_tag: 'native_cli',
)

native_test = executable(
    'howdy-test',
    'cli/test.cpp',
    'cli/test_main.cpp',
    include_directories: native_cpp_inc,
    link_with: native_cpp_lib,
    dependencies: [
        inih_cpp,
        nlohmann_json,
        opencv4,
    ],
    install: true,
    install_dir: join_paths(get_option('prefix'), get_option('libdir'), 'howdy'),
    install_tag: 'native_cli',
)

native_list = executable(
    'howdy-list',
    'cli/list.cpp',
    'cli/list_main.cpp',
    include_directories: native_cpp_inc,
    link_with: native_cpp_lib,
    dependencies: [
        inih_cpp,
        nlohmann_json,
        opencv4,
    ],
    install: true,
    install_dir: join_paths(get_option('prefix'), get_option('libdir'), 'howdy'),
    install_tag: 'native_cli',
)

native_remove = executable(
    'howdy-remove',
    'cli/remove.cpp',
    'cli/remove_main.cpp',
    include_directories: native_cpp_inc,
    link_with: native_cpp_lib,
    dependencies: [
        inih_cpp,
        nlohmann_json,
        opencv4,
    ],
    install: true,
    install_dir: join_paths(get_option('prefix'), get_option('libdir'), 'howdy'),
    install_tag: 'native_cli',
)

native_clear = executable(
    'howdy-clear',
    'cli/clear.cpp',
    'cli/clear_main.cpp',
    include_directories: native_cpp_inc,
    link_with: native_cpp_lib,
    dependencies: [
        inih_cpp,
        nlohmann_json,
        opencv4,
    ],
    install: true,
    install_dir: join_paths(get_option('prefix'), get_option('libdir'), 'howdy'),
    install_tag: 'native_cli',
)

native_snapshot = executable(
    'howdy-snapshot',
    'cli/snapshot.cpp',
    'cli/snapshot_main.cpp',
    include_directories: native_cpp_inc,
    link_with: native_cpp_lib,
    dependencies: [
        inih_cpp,
        nlohmann_json,
        opencv4,
    ],
    install: true,
    install_dir: join_paths(get_option('prefix'), get_option('libdir'), 'howdy'),
    install_tag: 'native_cli',
)

native_set = executable(
    'howdy-set',
    'cli/set.cpp',
    'cli/set_main.cpp',
    include_directories: native_cpp_inc,
    link_with: native_cpp_lib,
    dependencies: [
        inih_cpp,
        libcurl,
        nlohmann_json,
        opencv4,
    ],
    install: true,
    install_dir: join_paths(get_option('prefix'), get_option('libdir'), 'howdy'),
    install_tag: 'native_cli',
)

native_disable = executable(
    'howdy-disable',
    'cli/disable.cpp',
    'cli/disable_main.cpp',
    include_directories: native_cpp_inc,
    link_with: native_cpp_lib,
    dependencies: [
        inih_cpp,
        libcurl,
        nlohmann_json,
        opencv4,
    ],
    install: true,
    install_dir: join_paths(get_option('prefix'), get_option('libdir'), 'howdy'),
    install_tag: 'native_cli',
)

native_config = executable(
    'howdy-config',
    'cli/config.cpp',
    'cli/config_main.cpp',
    include_directories: native_cpp_inc,
    link_with: native_cpp_lib,
    dependencies: [
        inih_cpp,
        libcurl,
        nlohmann_json,
        opencv4,
    ],
    install: true,
    install_dir: join_paths(get_option('prefix'), get_option('libdir'), 'howdy'),
    install_tag: 'native_cli',
)

native_download_models = executable(
    'howdy-download-models',
    'cli/download_models.cpp',
    'cli/download_models_main.cpp',
    include_directories: native_cpp_inc,
    link_with: native_cpp_lib,
    dependencies: [
        inih_cpp,
        libcurl,
        openssl,
        nlohmann_json,
        opencv4,
    ],
    install: true,
    install_dir: join_paths(get_option('prefix'), get_option('libdir'), 'howdy'),
    install_tag: 'native_cli',
)

native_howdy = executable(
    'howdy',
    'howdy.cpp',
    include_directories: native_cpp_inc,
    dependencies: [],
    install: true,
    install_dir: get_option('bindir'),
    install_tag: 'native_cli',
)

test(
    'native-howdy-help',
    native_howdy,
    args: ['--help'],
)

test(
    'native-howdy-version',
    native_howdy,
    args: ['version'],
    should_fail: true,
)

test(
    'native-howdy-nonroot-path',
    native_howdy,
    args: ['-U', 'coverage-user', 'list'],
    should_fail: true,
)

test(
    'native-howdy-empty',
    native_howdy,
)

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
install_emptydir(confdir, install_mode: 'rwxr-x---', install_tag: 'config')
if not fs.exists(config_path)
    install_data('../../config/config.ini', install_dir: confdir, install_mode: 'rw-r-----', install_tag: 'config')
endif

install_emptydir(modelsdir, install_tag: 'models')

install_man('../howdy.1')
