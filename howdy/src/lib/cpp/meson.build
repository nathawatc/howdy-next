opencv4 = dependency('opencv4')
inih_cpp = dependency('INIReader', fallback: ['inih', 'INIReader_dep'])
nlohmann_json = dependency('nlohmann_json')
libcurl = dependency('libcurl')

native_paths_h = configure_file(
    input: 'config/paths.hpp.in',
    output: 'paths.hpp',
    configuration: native_cpp_conf,
)

native_cpp_inc = include_directories('.')

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
