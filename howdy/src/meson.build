opencv4 = dependency('opencv4', include_type: 'system')
inih_cpp = dependency('INIReader')
yyjson = dependency('yyjson', version: '>=0.12.0')
libcurl = dependency('libcurl')
openssl = dependency('openssl')
native_cpp_conf = configuration_data(paths_dict)

configure_file(
	input: '../include/config/paths.hpp.in',
	output: 'paths.hpp',
	configuration: native_cpp_conf,
)

native_cpp_inc = include_directories('.', '../include')

native_compare_logic_lib = static_library(
	'howdy_compare_logic',
	'compare_logic.cpp',
	include_directories: native_cpp_inc,
	dependencies: [
		opencv4,
	],
	install: false,
)

native_frame_processing_lib = static_library(
	'howdy_frame_processing',
	'frame_processing.cpp',
	include_directories: native_cpp_inc,
	dependencies: [
		opencv4,
	],
	install: false,
)

native_preview_engine_lib = static_library(
	'howdy_preview_engine',
	'preview_engine.cpp',
	include_directories: native_cpp_inc,
	dependencies: [
		opencv4,
	],
	install: false,
)

native_test_preview_lib = static_library(
	'howdy_test_preview',
	'cli/test_preview_session.cpp',
	'cli/test_preview_renderer.cpp',
	include_directories: native_cpp_inc,
	link_with: [
		native_preview_engine_lib,
		native_frame_processing_lib,
	],
	dependencies: [
		opencv4,
	],
	install: false,
)

native_compare_engine_lib = static_library(
	'howdy_compare_engine',
	'compare_engine.cpp',
	include_directories: native_cpp_inc,
	dependencies: [
		opencv4,
	],
	install: false,
)

native_compare_capture_session_lib = static_library(
	'howdy_compare_capture_session',
	'compare_capture_session.cpp',
	include_directories: native_cpp_inc,
	dependencies: [
		opencv4,
	],
	install: false,
)

native_cpp_sources = [
	'config/config_reader.cpp',
	'config/runtime_config.cpp',
	'config/config_schema.cpp',
	'config/config_utils.cpp',
	'config/config_validation.cpp',
	'config/runtime_paths.cpp',
	'core/face_detection.cpp',
	'core/face_encoding.cpp',
	'core/face_model.cpp',
	'recorders/video_capture.cpp',
	'storage/user_model_codec.cpp',
	'storage/user_model_readiness.cpp',
	'storage/user_model_store.cpp',
	'storage/user_models.cpp',
]

native_cpp_lib = static_library(
	'howdy_native_cpp',
	native_cpp_sources,
	include_directories: native_cpp_inc,
	dependencies: [
		inih_cpp,
		libcurl,
		yyjson,
		opencv4,
	],
	install: false,
)

native_compare = executable(
	'howdy-compare',
	'compare_args.cpp',
	'compare.cpp',
	include_directories: native_cpp_inc,
	link_with: [
		native_compare_capture_session_lib,
		native_cpp_lib,
		native_compare_engine_lib,
		native_frame_processing_lib,
		native_compare_logic_lib,
	],
	dependencies: [
		inih_cpp,
		libcurl,
		yyjson,
		opencv4,
	],
	install: true,
	install_dir: helperdir,
	install_tag: 'native_helpers',
)

native_auth_helper = executable(
	'howdy-auth-helper',
	'auth_helper.cpp',
	'auth_helper_runtime.cpp',
	'config/runtime_paths.cpp',
	'storage/user_model_readiness.cpp',
	include_directories: native_cpp_inc,
	install: true,
	install_dir: helperdir,
	install_mode: 'rwsr-xr-x',
	install_tag: 'native_helpers',
)

test(
	'native-compare-help',
	native_compare,
	args: ['--help'],
)

test(
	'native-auth-helper-help',
	native_auth_helper,
	args: ['--help'],
)

test(
	'native-auth-helper-empty',
	native_auth_helper,
	expected_fail: true,
)

test(
	'native-auth-helper-unknown-command',
	native_auth_helper,
	args: ['unknown', 'alice'],
	expected_fail: true,
)

test(
	'native-auth-helper-prepare-guard',
	native_auth_helper,
	args: ['prepare', '../alice'],
	expected_fail: true,
)

test(
	'native-auth-helper-cleanup-guard',
	native_auth_helper,
	args: ['cleanup', '/tmp/not-howdy-runtime'],
	expected_fail: true,
)

native_auth_helper_test = executable(
	'howdy-auth-helper-test',
	'auth_helper.cpp',
	'auth_helper_runtime.cpp',
	'config/runtime_paths.cpp',
	'storage/user_model_readiness.cpp',
	'tests/auth_helper_test.cpp',
	include_directories: native_cpp_inc,
	cpp_args: ['-DHOWDY_AUTH_HELPER_TESTING'],
	install: false,
)

test(
	'native-auth-helper',
	native_auth_helper_test,
)

test(
	'native-compare-unknown-arg',
	native_compare,
	args: ['--unknown'],
	expected_fail: true,
)

test(
	'native-compare-missing-user',
	native_compare,
	expected_fail: true,
)

test(
	'native-compare-no-model',
	native_compare,
	args: ['coverage-user'],
	env: ['HOWDY_USER_MODELS_DIR=' + meson.current_build_dir() / 'compare-test-models-empty'],
	expected_fail: true,
)

native_compare_args_test = executable(
	'howdy-compare-args-test',
	'compare_args.cpp',
	'tests/compare_args_test.cpp',
	include_directories: native_cpp_inc,
	dependencies: [
		inih_cpp,
		libcurl,
		yyjson,
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
	'tests/compare_logic_test.cpp',
	include_directories: native_cpp_inc,
	link_with: native_compare_logic_lib,
	dependencies: [
		opencv4,
	],
	install: false,
)

test(
	'native-compare-logic',
	native_compare_logic_test,
)

native_frame_processing_test = executable(
	'howdy-frame-processing-test',
	'tests/frame_processing_test.cpp',
	include_directories: native_cpp_inc,
	link_with: [
		native_frame_processing_lib,
		native_compare_logic_lib,
	],
	dependencies: [
		opencv4,
	],
	install: false,
)

test(
	'native-frame-processing',
	native_frame_processing_test,
)

native_compare_engine_test = executable(
	'howdy-compare-engine-test',
	'tests/compare_engine_test.cpp',
	include_directories: native_cpp_inc,
	link_with: [
		native_compare_engine_lib,
		native_frame_processing_lib,
		native_compare_logic_lib,
	],
	dependencies: [
		opencv4,
	],
	install: false,
)

test(
	'native-compare-engine',
	native_compare_engine_test,
)

native_compare_capture_session_test = executable(
	'howdy-compare-capture-session-test',
	'tests/compare_capture_session_test.cpp',
	include_directories: native_cpp_inc,
	link_with: [
		native_compare_capture_session_lib,
		native_cpp_lib,
	],
	dependencies: [
		inih_cpp,
		libcurl,
		yyjson,
		opencv4,
	],
	install: false,
)

test(
	'native-compare-capture-session',
	native_compare_capture_session_test,
)

native_face_matching_test = executable(
	'howdy-face-matching-test',
	'tests/face_matching_test.cpp',
	include_directories: native_cpp_inc,
	install: false,
)

test(
	'native-face-matching',
	native_face_matching_test,
)

native_face_detection_test = executable(
	'howdy-face-detection-test',
	'tests/face_detection_test.cpp',
	'core/face_detection.cpp',
	include_directories: native_cpp_inc,
	dependencies: [
		opencv4,
	],
	install: false,
)

test(
	'native-face-detection',
	native_face_detection_test,
)

native_face_encoding_test = executable(
	'howdy-face-encoding-test',
	'tests/face_encoding_test.cpp',
	'core/face_encoding.cpp',
	include_directories: native_cpp_inc,
	dependencies: [
		opencv4,
	],
	install: false,
)

test(
	'native-face-encoding',
	native_face_encoding_test,
)

native_face_model_test = executable(
	'howdy-face-model-test',
	'tests/face_model_test.cpp',
	include_directories: native_cpp_inc,
	link_with: native_cpp_lib,
	dependencies: [
		inih_cpp,
		libcurl,
		yyjson,
		opencv4,
	],
	install: false,
)

test(
	'native-face-model',
	native_face_model_test,
)

native_config_utils_test = executable(
	'howdy-config-utils-test',
	'tests/config_utils_test.cpp',
	include_directories: native_cpp_inc,
	link_with: native_cpp_lib,
	dependencies: [
		inih_cpp,
		libcurl,
		yyjson,
		opencv4,
	],
	install: false,
)

test(
	'native-config-utils',
	native_config_utils_test,
)

native_file_security_test = executable(
	'howdy-file-security-test',
	'tests/file_security_test.cpp',
	include_directories: native_cpp_inc,
	install: false,
)

test(
	'native-file-security',
	native_file_security_test,
)

native_fd_io_test = executable(
	'howdy-fd-io-test',
	'tests/fd_io_test.cpp',
	include_directories: native_cpp_inc,
	install: false,
)

test(
	'native-fd-io',
	native_fd_io_test,
)

native_model_file_test = executable(
	'howdy-model-file-test',
	'tests/model_file_test.cpp',
	include_directories: native_cpp_inc,
	install: false,
)

test(
	'native-model-file',
	native_model_file_test,
)

native_capture_device_path_test = executable(
	'howdy-capture-device-path-test',
	'tests/capture_device_path_test.cpp',
	include_directories: native_cpp_inc,
	install: false,
)

test(
	'native-capture-device-path',
	native_capture_device_path_test,
)

native_video_capture_test = executable(
	'howdy-video-capture-test',
	'recorders/video_capture.cpp',
	'tests/video_capture_test.cpp',
	include_directories: native_cpp_inc,
	link_with: native_cpp_lib,
	dependencies: [
		opencv4,
	],
	install: false,
)

test(
	'native-video-capture',
	native_video_capture_test,
)

native_enrollment_capture_test = executable(
	'howdy-enrollment-capture-test',
	'tests/enrollment_capture_test.cpp',
	include_directories: native_cpp_inc,
	link_with: [
		native_frame_processing_lib,
		native_compare_logic_lib,
	],
	dependencies: [
		opencv4,
	],
	install: false,
)

test(
	'native-enrollment-capture',
	native_enrollment_capture_test,
)

packaged_config_cpp_arg = '-DHOWDY_PACKAGED_CONFIG_PATH="' + meson.project_source_root() / 'config/config.ini' + '"'

native_config_validation_test = executable(
	'howdy-config-validation-test',
	'tests/config_validation_test.cpp',
	include_directories: native_cpp_inc,
	cpp_args: [packaged_config_cpp_arg],
	link_with: native_cpp_lib,
	dependencies: [
		inih_cpp,
		libcurl,
		yyjson,
		opencv4,
	],
	install: false,
)

test(
	'native-config-validation',
	native_config_validation_test,
)

native_user_models_test = executable(
	'howdy-user-models-test',
	'tests/user_models_test.cpp',
	include_directories: native_cpp_inc,
	link_with: native_cpp_lib,
	dependencies: [
		inih_cpp,
		libcurl,
		yyjson,
		opencv4,
	],
	install: false,
)

test(
	'native-user-models',
	native_user_models_test,
)

native_user_model_codec_test = executable(
	'howdy-user-model-codec-test',
	'storage/user_model_codec.cpp',
	'tests/user_model_codec_test.cpp',
	include_directories: native_cpp_inc,
	dependencies: [
		yyjson,
	],
	install: false,
)

test(
	'native-user-model-codec',
	native_user_model_codec_test,
)

native_clear_cli_test = executable(
	'howdy-clear-cli-test',
	'cli/clear.cpp',
	'tests/clear_cli_test.cpp',
	include_directories: native_cpp_inc,
	link_with: native_cpp_lib,
	dependencies: [
		inih_cpp,
		libcurl,
		yyjson,
		opencv4,
	],
	install: false,
)

test(
	'native-clear-cli',
	native_clear_cli_test,
)

native_config_cli_test = executable(
	'howdy-config-cli-test',
	'cli/config.cpp',
	'tests/config_cli_test.cpp',
	include_directories: native_cpp_inc,
	link_args: ['-Wl,--wrap=initgroups'],
	link_with: native_cpp_lib,
	dependencies: [
		inih_cpp,
		libcurl,
		yyjson,
		opencv4,
	],
	install: false,
)

test(
	'native-config-cli',
	native_config_cli_test,
)

native_disable_cli_test = executable(
	'howdy-disable-cli-test',
	'cli/disable.cpp',
	'tests/disable_cli_test.cpp',
	include_directories: native_cpp_inc,
	link_with: native_cpp_lib,
	dependencies: [
		inih_cpp,
		libcurl,
		yyjson,
		opencv4,
	],
	install: false,
)

test(
	'native-disable-cli',
	native_disable_cli_test,
)

native_set_cli_test = executable(
	'howdy-set-cli-test',
	'cli/set.cpp',
	'tests/set_cli_test.cpp',
	include_directories: native_cpp_inc,
	link_with: native_cpp_lib,
	dependencies: [
		inih_cpp,
		yyjson,
		opencv4,
	],
	install: false,
)

test(
	'native-set-cli',
	native_set_cli_test,
)

native_list_cli_test = executable(
	'howdy-list-cli-test',
	'cli/list.cpp',
	'tests/list_cli_test.cpp',
	include_directories: native_cpp_inc,
	link_with: native_cpp_lib,
	dependencies: [
		inih_cpp,
		yyjson,
		opencv4,
	],
	install: false,
)

test(
	'native-list-cli',
	native_list_cli_test,
)

native_add_cli_test = executable(
	'howdy-add-cli-test',
	'cli/add.cpp',
	'tests/add_cli_test.cpp',
	include_directories: native_cpp_inc,
	link_with: [
		native_cpp_lib,
		native_frame_processing_lib,
		native_compare_logic_lib,
	],
	dependencies: [
		inih_cpp,
		yyjson,
		opencv4,
	],
	install: false,
)

test(
	'native-add-cli',
	native_add_cli_test,
)

native_remove_cli_test = executable(
	'howdy-remove-cli-test',
	'cli/remove.cpp',
	'tests/remove_cli_test.cpp',
	include_directories: native_cpp_inc,
	link_with: native_cpp_lib,
	dependencies: [
		inih_cpp,
		yyjson,
		opencv4,
	],
	install: false,
)

test(
	'native-remove-cli',
	native_remove_cli_test,
)

native_test_cli_test = executable(
	'howdy-test-cli-test',
	'cli/test.cpp',
	'tests/test_cli_test.cpp',
	include_directories: native_cpp_inc,
	link_with: [
		native_cpp_lib,
		native_test_preview_lib,
		native_preview_engine_lib,
		native_frame_processing_lib,
	],
	dependencies: [
		inih_cpp,
		yyjson,
		opencv4,
	],
	install: false,
)

test(
	'native-test-cli',
	native_test_cli_test,
)

native_test_preview_session_test = executable(
	'howdy-test-preview-session-test',
	'cli/test.cpp',
	'tests/test_preview_session_test.cpp',
	include_directories: native_cpp_inc,
	link_with: [
		native_cpp_lib,
		native_test_preview_lib,
		native_preview_engine_lib,
		native_frame_processing_lib,
	],
	dependencies: [opencv4],
	install: false,
)

test(
	'native-test-preview-session',
	native_test_preview_session_test,
)

native_preview_engine_test = executable(
	'howdy-preview-engine-test',
	'tests/preview_engine_test.cpp',
	include_directories: native_cpp_inc,
	link_with: [
		native_cpp_lib,
		native_preview_engine_lib,
		native_frame_processing_lib,
	],
	dependencies: [opencv4],
	install: false,
)

test(
	'native-preview-engine',
	native_preview_engine_test,
)

native_snapshot_cli_test = executable(
	'howdy-snapshot-cli-test',
	'cli/snapshot.cpp',
	'tests/snapshot_cli_test.cpp',
	include_directories: native_cpp_inc,
	link_with: native_cpp_lib,
	dependencies: [
		inih_cpp,
		yyjson,
		opencv4,
	],
	install: false,
)

test(
	'native-snapshot-cli',
	native_snapshot_cli_test,
)

native_snapshot_writer_test = executable(
	'howdy-snapshot-writer-test',
	'cli/snapshot.cpp',
	'tests/snapshot_writer_test.cpp',
	include_directories: native_cpp_inc,
	link_with: native_cpp_lib,
	dependencies: [
		inih_cpp,
		yyjson,
		opencv4,
	],
	install: false,
)

test(
	'native-snapshot-writer',
	native_snapshot_writer_test,
)

native_download_models_test = executable(
	'howdy-download-models-test',
	'cli/download_models.cpp',
	'config/runtime_paths.cpp',
	'tests/download_models_test.cpp',
	include_directories: native_cpp_inc,
	dependencies: [
		libcurl,
		openssl,
		opencv4,
	],
	install: false,
)

test(
	'native-download-models',
	native_download_models_test,
)

native_user_names_test = executable(
	'howdy-user-names-test',
	'tests/user_names_test.cpp',
	include_directories: native_cpp_inc,
	install: false,
)

test(
	'native-user-names',
	native_user_names_test,
)

native_runtime_paths_test = executable(
	'howdy-runtime-paths-test',
	'tests/runtime_paths_test.cpp',
	include_directories: native_cpp_inc,
	link_with: native_cpp_lib,
	dependencies: [
		inih_cpp,
		libcurl,
		yyjson,
		opencv4,
	],
	install: false,
)

test(
	'native-runtime-paths',
	native_runtime_paths_test,
)

native_runtime_config_load_test = executable(
	'howdy-runtime-config-load-test',
	'tests/runtime_config_load_test.cpp',
	include_directories: native_cpp_inc,
	link_with: native_cpp_lib,
	dependencies: [
		inih_cpp,
		libcurl,
		yyjson,
		opencv4,
	],
	install: false,
)

test(
	'native-runtime-config-load',
	native_runtime_config_load_test,
	env: ['TMPDIR=' + meson.current_build_dir()],
)

native_runtime_config_test = executable(
	'howdy-runtime-config-test',
	'tests/runtime_config_test.cpp',
	include_directories: native_cpp_inc,
	link_with: native_cpp_lib,
	dependencies: [
		inih_cpp,
		libcurl,
		yyjson,
		opencv4,
	],
	install: false,
)

test(
	'native-runtime-config',
	native_runtime_config_test,
	env: ['TMPDIR=' + meson.current_build_dir()],
)

native_invoking_user_env_test = executable(
	'howdy-invoking-user-env-test',
	'tests/invoking_user_env_test.cpp',
	include_directories: native_cpp_inc,
	dependencies: [
		inih_cpp,
		libcurl,
		yyjson,
		opencv4,
	],
	install: false,
)

test(
	'native-invoking-user-env',
	native_invoking_user_env_test,
)

native_config_reader_test = executable(
	'howdy-config-reader-test',
	'tests/config_reader_test.cpp',
	include_directories: native_cpp_inc,
	link_with: native_cpp_lib,
	dependencies: [
		inih_cpp,
		libcurl,
		yyjson,
		opencv4,
	],
	install: false,
)

test(
	'native-config-reader',
	native_config_reader_test,
)

native_cli_lib = static_library(
	'howdy_cli',
	'howdy.cpp',
	'cli/add.cpp',
	'cli/clear.cpp',
	'cli/config.cpp',
	'cli/disable.cpp',
	'cli/download_models.cpp',
	'cli/list.cpp',
	'cli/remove.cpp',
	'cli/set.cpp',
	'cli/snapshot.cpp',
	'cli/test.cpp',
	include_directories: native_cpp_inc,
	link_with: [
		native_cpp_lib,
		native_test_preview_lib,
		native_preview_engine_lib,
		native_frame_processing_lib,
		native_compare_logic_lib,
	],
	dependencies: [
		inih_cpp,
		libcurl,
		openssl,
		yyjson,
		opencv4,
	],
	install: false,
)

native_howdy = executable(
	'howdy',
	'howdy_main.cpp',
	include_directories: native_cpp_inc,
	link_with: native_cli_lib,
	dependencies: [
		inih_cpp,
		libcurl,
		openssl,
		yyjson,
		opencv4,
	],
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
)

test(
	'native-howdy-nonroot-path',
	native_howdy,
	args: ['-U', 'coverage-user', 'list'],
	expected_fail: true,
)

test(
	'native-howdy-empty',
	native_howdy,
)

native_howdy_dispatch_test = executable(
	'howdy-dispatch-test',
	'tests/howdy_dispatch_test.cpp',
	include_directories: native_cpp_inc,
	link_with: native_cli_lib,
	dependencies: [
		inih_cpp,
		libcurl,
		openssl,
		yyjson,
		opencv4,
	],
	install: false,
)

test(
	'native-howdy-dispatch',
	native_howdy_dispatch_test,
)

test(
	'native-howdy-install-layout',
	find_program('sh'),
	args: [
		files('tests/install_layout_test.sh'),
		meson.project_build_root(),
		get_option('prefix'),
		get_option('bindir'),
		get_option('libexecdir'),
	],
)

autocomplete = configure_file(
	input: 'autocomplete/howdy.in',
	output: 'autocomplete',
	configuration: configuration_data({'config_path': config_path}),
)
install_data(
	autocomplete,
	install_dir: prefix / datadir / 'bash-completion' / 'completions',
	install_mode: 'rw-r--r--',
	install_tag: 'bash_completion',
	rename: 'howdy',
)

fs = import('fs')
install_emptydir(confdir, install_mode: 'rwxr-x---', install_tag: 'config')
if not fs.exists(config_path)
	install_data('../../config/config.ini', install_dir: confdir, install_mode: 'rw-r-----', install_tag: 'config')
endif

install_emptydir(modelsdir, install_tag: 'models')

install_man('../howdy.1', '../../pam/pam_howdy.8')
