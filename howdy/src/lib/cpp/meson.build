opencv4 = dependency('opencv4')
inih_cpp = dependency('INIReader', fallback: ['inih', 'INIReader_dep'])
nlohmann_json = dependency('nlohmann_json')

native_paths_h = configure_file(
    input: 'config/paths.hpp.in',
    output: 'paths.hpp',
    configuration: native_cpp_conf,
)

native_cpp_inc = include_directories('.')

native_cpp_sources = [
    'config/config_reader.cpp',
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
