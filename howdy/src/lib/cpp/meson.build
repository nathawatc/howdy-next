opencv4 = dependency('opencv4')
inih_cpp = dependency('INIReader', fallback: ['inih', 'INIReader_dep'])

native_paths_h = configure_file(
    input: 'config/paths.hpp.in',
    output: 'paths.hpp',
    configuration: native_cpp_conf,
)

native_cpp_inc = include_directories('.')

native_cpp_sources = [
    'config/config_reader.cpp',
    'recorders/video_capture.cpp',
]

native_cpp_lib = static_library(
    'howdy_native_cpp',
    native_cpp_sources,
    include_directories: native_cpp_inc,
    dependencies: [
        inih_cpp,
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
        opencv4,
    ],
    install: false,
)

test(
    'native-capture-smoke-help',
    native_capture_smoke,
    args: ['--help'],
)
