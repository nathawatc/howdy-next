#include "cli/test_internal.hpp"

#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

	struct StreamRedirect {
		std::ostream   &output;
		std::streambuf *old_buffer;

		StreamRedirect(std::ostream &stream, std::streambuf *new_buffer)
		    : output(stream)
		    , old_buffer(stream.rdbuf(new_buffer)) {}

		~StreamRedirect() {
			output.rdbuf(old_buffer);
		}
	};

	struct TestCliTestContext {
		howdy::native::RuntimeConfigLoadResult config_result;

		bool        face_model_ok     = true;
		std::string face_model_error  = "face model failed";
		bool        graphical_display = true;
		bool        camera_open_ok    = true;
		std::string camera_open_error = "camera open failed";
		bool        camera_read_ok    = true;
		bool        gui_user_ok       = true;

		int load_calls     = 0;
		int preview_calls  = 0;
		int face_calls     = 0;
		int display_calls  = 0;
		int open_calls     = 0;
		int read_calls     = 0;
		int gui_calls      = 0;
		int gui_init_calls = 0;

		std::vector<std::string> sequence;

		std::string                  preview_user;
		std::string                  preview_device_path;
		std::string                  face_user;
		std::string                  opened_device_path;
		howdy::native::RuntimeConfig preview_config;
		howdy::native::RuntimeConfig open_config;
	};

	auto expect(bool condition, const std::string &message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << "\n";
			return false;
		}
		return true;
	}

	auto expect_sequence(const std::vector<std::string> &actual,
	                     const std::vector<std::string> &expected, const std::string &message)
	    -> bool {
		if (actual == expected) {
			return true;
		}
		std::cerr << "FAIL: " << message << "\n";
		std::cerr << "  actual:";
		for (const auto &step : actual) {
			std::cerr << " " << step;
		}
		std::cerr << "\n  expected:";
		for (const auto &step : expected) {
			std::cerr << " " << step;
		}
		std::cerr << "\n";
		return false;
	}

	auto valid_config_load_result() -> howdy::native::RuntimeConfigLoadResult {
		howdy::native::RuntimeConfig config;
		config.video.dark_threshold = 41.0F;
		config.video.device_path    = "/dev/video-default";
		return howdy::native::RuntimeConfigLoadResult{
		    .ok     = true,
		    .status = howdy::native::RuntimeConfigLoadStatus::kOk,
		    .config = config,
		};
	}

	auto invalid_config_load_result() -> howdy::native::RuntimeConfigLoadResult {
		return howdy::native::RuntimeConfigLoadResult{
		    .ok            = false,
		    .status        = howdy::native::RuntimeConfigLoadStatus::kInvalidRuntimeValue,
		    .error_message = "invalid runtime config",
		};
	}

	auto load_runtime_config_callback(void *raw_context) -> howdy::native::RuntimeConfigLoadResult {
		auto *context = static_cast<TestCliTestContext *>(raw_context);
		++context->load_calls;
		return context->config_result;
	}

	auto face_model_ready_callback(void *raw_context, const howdy::native::RuntimeConfig &config,
	                               const std::string &user)
	    -> howdy::native::test_internal::TestPreflightOperationResult {
		(void)config;
		auto *context = static_cast<TestCliTestContext *>(raw_context);
		++context->face_calls;
		context->sequence.push_back("face");
		context->face_user = user;
		if (!context->face_model_ok) {
			return howdy::native::test_internal::TestPreflightOperationResult{
			    .error_message = context->face_model_error,
			};
		}
		return howdy::native::test_internal::TestPreflightOperationResult{.ok = true};
	}

	auto has_graphical_display_callback(void *raw_context) -> bool {
		auto *context = static_cast<TestCliTestContext *>(raw_context);
		++context->display_calls;
		context->sequence.push_back("display");
		return context->graphical_display;
	}

	auto open_camera_callback(void *raw_context, const howdy::native::RuntimeConfig &config,
	                          const std::string &device_path)
	    -> howdy::native::test_internal::TestPreflightOperationResult {
		auto *context = static_cast<TestCliTestContext *>(raw_context);
		++context->open_calls;
		context->sequence.push_back("open");
		context->open_config        = config;
		context->opened_device_path = device_path;
		if (!context->camera_open_ok) {
			return howdy::native::test_internal::TestPreflightOperationResult{
			    .error_message = context->camera_open_error,
			};
		}
		return howdy::native::test_internal::TestPreflightOperationResult{.ok = true};
	}

	auto read_camera_callback(void *raw_context) -> bool {
		auto *context = static_cast<TestCliTestContext *>(raw_context);
		++context->read_calls;
		context->sequence.push_back("read");
		return context->camera_read_ok;
	}

	auto switch_gui_user_callback(void *raw_context) -> bool {
		auto *context = static_cast<TestCliTestContext *>(raw_context);
		++context->gui_calls;
		context->sequence.push_back("switch_gui_user");
		return context->gui_user_ok;
	}

	void initialize_gui_callback(void *raw_context) {
		auto *context = static_cast<TestCliTestContext *>(raw_context);
		++context->gui_init_calls;
		context->sequence.push_back("initialize_gui");
	}

	auto preflight_dependencies(TestCliTestContext &context)
	    -> howdy::native::test_internal::TestPreviewPreflightDependencies {
		return howdy::native::test_internal::TestPreviewPreflightDependencies{
		    .context               = &context,
		    .face_model_ready      = face_model_ready_callback,
		    .has_graphical_display = has_graphical_display_callback,
		    .open_camera           = open_camera_callback,
		    .read_camera           = read_camera_callback,
		    .switch_gui_user       = switch_gui_user_callback,
		    .initialize_gui        = initialize_gui_callback,
		};
	}

	auto run_preview_callback(void *raw_context, const howdy::native::RuntimeConfig &config,
	                          const std::string &user, const std::string &device_path)
	    -> howdy::native::test_internal::TestPreviewResult {
		auto *context = static_cast<TestCliTestContext *>(raw_context);
		++context->preview_calls;
		context->preview_config      = config;
		context->preview_user        = user;
		context->preview_device_path = device_path;
		return howdy::native::test_internal::run_preview_preflight(
		    config, user, device_path, preflight_dependencies(*context));
	}

	auto test_dependencies(TestCliTestContext &context)
	    -> howdy::native::test_internal::TestDependencies {
		return howdy::native::test_internal::TestDependencies{
		    .context             = &context,
		    .load_runtime_config = load_runtime_config_callback,
		    .run_preview         = run_preview_callback,
		};
	}

	auto run_test_with_dependencies(howdy::native::test_internal::TestDependencies dependencies,
	                                std::vector<std::string> arguments) -> int {
		std::vector<char *> argv;
		argv.reserve(arguments.size());
		for (auto &argument : arguments) {
			argv.push_back(argument.data());
		}
		return howdy::native::test_internal::test_main_with_dependencies(
		    static_cast<int>(argv.size()), argv.data(), dependencies);
	}

	auto run_test(TestCliTestContext &context, std::vector<std::string> arguments) -> int {
		return run_test_with_dependencies(test_dependencies(context), std::move(arguments));
	}

	auto make_success_context() -> TestCliTestContext {
		TestCliTestContext context;
		context.config_result = valid_config_load_result();
		return context;
	}

	auto invalid_runtime_config_stops_before_preview() -> bool {
		auto context          = make_success_context();
		context.config_result = invalid_config_load_result();
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = run_test(context, {"howdy-test"});

		bool ok = true;
		ok &= expect(result == 1, "invalid runtime config returns 1");
		ok &= expect(context.load_calls == 1, "invalid runtime config loads once");
		ok &= expect(context.preview_calls == 0, "invalid runtime config skips preview");
		ok &= expect(context.face_calls == 0, "invalid runtime config skips face preflight");
		ok &= expect(error.str().contains("invalid runtime config"),
		             "invalid runtime config writes config error");
		return ok;
	}

	auto face_model_failure_returns_error() -> bool {
		auto context             = make_success_context();
		context.face_model_ok    = false;
		context.face_model_error = "face model failed";
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = run_test(context, {"howdy-test", "alice"});

		bool ok = true;
		ok &= expect(result == 1, "face model failure returns 1");
		ok &= expect(context.load_calls == 1, "face model failure loads once");
		ok &= expect(context.preview_calls == 1, "face model failure runs preview once");
		ok &= expect(context.face_calls == 1, "face model failure checks face model once");
		ok &= expect(context.face_user == "alice", "face model preflight receives user");
		ok &= expect(context.display_calls == 0, "face model failure skips display probe");
		ok &= expect(context.open_calls == 0, "face model failure skips camera open");
		ok &= expect(context.gui_init_calls == 0, "face model failure skips GUI init");
		ok &= expect(error.str().contains("face model failed"),
		             "face model failure writes fixed message");
		return ok;
	}

	auto missing_graphical_environment_prints_diagnostic() -> bool {
		auto context              = make_success_context();
		context.graphical_display = false;
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = run_test(context, {"howdy-test"});

		const auto error_text = error.str();
		bool       ok         = true;
		ok &= expect(result == 1, "missing graphical environment returns 1");
		ok &= expect(context.preview_calls == 1, "missing graphical environment runs preview once");
		ok &=
		    expect(context.face_calls == 1, "missing graphical environment checks face model once");
		ok &=
		    expect(context.display_calls == 1, "missing graphical environment probes display once");
		ok &= expect(context.open_calls == 0, "missing graphical environment skips camera open");
		ok &= expect(context.gui_init_calls == 0, "missing graphical environment skips GUI init");
		ok &= expect(error_text.contains("Cannot open the interactive test preview because no "
		                                 "graphical display environment is available."),
		             "missing graphical environment writes primary diagnostic");
		ok &= expect(error_text.contains("sudo howdy snapshot"),
		             "missing graphical environment writes headless fallback");
		return ok;
	}

	auto camera_open_failure_prints_device_and_capture_error() -> bool {
		auto context              = make_success_context();
		context.camera_open_ok    = false;
		context.camera_open_error = "camera open failed";
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = run_test(context, {"howdy-test"});

		const auto error_text = error.str();
		bool       ok         = true;
		ok &= expect(result == 1, "camera open failure returns 1");
		ok &= expect(context.preview_calls == 1, "camera open failure runs preview once");
		ok &= expect(context.open_calls == 1, "camera open failure opens camera once");
		ok &= expect(context.opened_device_path == "/dev/video-default",
		             "camera open failure uses configured device");
		ok &= expect(context.gui_calls == 0, "camera open failure skips GUI user switch");
		ok &= expect(context.gui_init_calls == 0, "camera open failure skips GUI init");
		ok &= expect(context.read_calls == 0, "camera open failure skips camera read");
		ok &= expect(error_text.contains("Failed to open camera device: /dev/video-default"),
		             "camera open failure writes device path");
		ok &= expect(error_text.contains("Error: camera open failed"),
		             "camera open failure writes capture error");
		return ok;
	}

	auto camera_read_failure_prints_diagnostic() -> bool {
		auto context           = make_success_context();
		context.camera_read_ok = false;
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = run_test(context, {"howdy-test"});

		bool ok = true;
		ok &= expect(result == 1, "camera read failure returns 1");
		ok &= expect(context.preview_calls == 1, "camera read failure runs preview once");
		ok &= expect(context.open_calls == 1, "camera read failure opens camera once");
		ok &= expect(context.gui_calls == 1, "camera read failure switches GUI user once");
		ok &= expect(context.gui_init_calls == 1, "camera read failure initializes GUI once");
		ok &= expect(context.read_calls == 1, "camera read failure reads camera once");
		ok &= expect(error.str().contains("Failed to read frame from camera"),
		             "camera read failure writes diagnostic");
		return ok;
	}

	auto gui_user_switch_failure_prints_diagnostic() -> bool {
		auto context        = make_success_context();
		context.gui_user_ok = false;
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = run_test(context, {"howdy-test"});

		const auto error_text = error.str();
		bool       ok         = true;
		ok &= expect(result == 1, "GUI user switch failure returns 1");
		ok &= expect(context.preview_calls == 1, "GUI user switch failure runs preview once");
		ok &= expect(context.open_calls == 1, "GUI user switch failure opens camera once");
		ok &= expect(context.gui_calls == 1, "GUI user switch failure switches GUI user once");
		ok &= expect(context.gui_init_calls == 0, "GUI user switch failure skips GUI init");
		ok &= expect(context.read_calls == 0, "GUI user switch failure skips camera read");
		ok &= expect(error_text.contains("Failed to switch GUI session to the invoking user"),
		             "GUI user switch failure writes primary diagnostic");
		ok &= expect(error_text.contains(
		                 "Run this command from your desktop session through sudo/doas/pkexec"),
		             "GUI user switch failure writes invocation diagnostic");
		return ok;
	}

	auto successful_preview_returns_zero() -> bool {
		auto               context = make_success_context();
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = run_test(context, {"howdy-test", "alice"});

		bool ok = true;
		ok &= expect(result == 0, "successful preview returns 0");
		ok &= expect(context.load_calls == 1, "successful preview loads once");
		ok &= expect(context.preview_calls == 1, "successful preview runs preview once");
		ok &= expect(context.face_calls == 1, "successful preview checks face model once");
		ok &= expect(context.display_calls == 1, "successful preview probes display once");
		ok &= expect(context.open_calls == 1, "successful preview opens camera once");
		ok &= expect(context.gui_calls == 1, "successful preview switches GUI user once");
		ok &= expect(context.gui_init_calls == 1, "successful preview initializes GUI once");
		ok &= expect(context.read_calls == 1, "successful preview reads camera once");
		ok &= expect(context.preview_user == "alice", "successful preview passes user argument");
		ok &= expect(context.preview_config.video.dark_threshold == 41.0F,
		             "successful preview passes runtime config");
		ok &= expect(error.str().empty(), "successful preview writes no failure diagnostic");
		return ok;
	}

	auto configured_device_default_is_used_for_camera_open() -> bool {
		auto               context = make_success_context();
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = run_test(context, {"howdy-test"});

		bool ok = true;
		ok &= expect(result == 0, "configured device default returns 0");
		ok &= expect(context.open_calls == 1, "configured device default opens camera once");
		ok &= expect(context.preview_device_path.empty(),
		             "configured device default has no CLI override");
		ok &= expect(context.opened_device_path == "/dev/video-default",
		             "configured device default is passed to camera open");
		ok &= expect(error.str().empty(), "configured device default writes no failure diagnostic");
		return ok;
	}

	auto device_override_is_used_for_camera_open() -> bool {
		auto               context = make_success_context();
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = run_test(context, {"howdy-test", "--device", "/dev/video1"});

		bool ok = true;
		ok &= expect(result == 0, "device override returns 0");
		ok &= expect(context.open_calls == 1, "device override opens camera once");
		ok &= expect(context.preview_device_path == "/dev/video1",
		             "device override is passed to preview callback");
		ok &= expect(context.opened_device_path == "/dev/video1",
		             "device override is passed to camera open");
		ok &= expect(error.str().empty(), "device override writes no failure diagnostic");
		return ok;
	}

	auto gui_initialization_runs_before_first_camera_read() -> bool {
		auto               context = make_success_context();
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = run_test(context, {"howdy-test"});

		bool ok = true;
		ok &= expect(result == 0, "GUI/read sequence returns 0");
		ok &= expect_sequence(
		    context.sequence,
		    {"face", "display", "open", "switch_gui_user", "initialize_gui", "read"},
		    "GUI initialization runs before first camera read");
		ok &= expect(error.str().empty(), "GUI/read sequence writes no failure diagnostic");
		return ok;
	}

	auto graphical_environment_helper_checks_display_values() -> bool {
		namespace test_internal = howdy::native::test_internal;

		bool ok = true;
		ok &= expect(!test_internal::has_graphical_display_environment("", "", ""),
		             "empty display values are not graphical");
		ok &= expect(test_internal::has_graphical_display_environment(":0", "", ""),
		             "DISPLAY enables graphical environment");
		ok &= expect(!test_internal::has_graphical_display_environment("", "wayland-0", ""),
		             "Wayland display without runtime dir is not graphical");
		ok &= expect(
		    test_internal::has_graphical_display_environment("", "wayland-0", "/run/user/1000"),
		    "Wayland display with runtime dir is graphical");
		ok &= expect(!test_internal::has_graphical_display_environment("", "", "/run/user/1000"),
		             "empty DISPLAY does not count as graphical");
		return ok;
	}

	auto missing_preflight_dependency_callbacks_fail_closed() -> bool {
		auto context      = make_success_context();
		auto dependencies = preflight_dependencies(context);

		auto run_missing_preflight = [&](auto clear_callback, const std::string &message) -> bool {
			auto missing_dependencies = dependencies;
			clear_callback(missing_dependencies);
			const auto result = howdy::native::test_internal::run_preview_preflight(
			    *context.config_result.config, "", "", missing_dependencies);
			return expect(result.status ==
			                  howdy::native::test_internal::TestPreviewStatus::kFaceModelError,
			              message + " returns face model error") &&
			       expect(
			           result.error_message.contains("missing test preview preflight dependency"),
			           message + " writes internal error") &&
			       expect(context.sequence.empty(), message + " skips all preflight operations");
		};

		bool ok = true;
		ok &= run_missing_preflight(
		    [](auto &missing_dependencies) {
			    missing_dependencies.face_model_ready = nullptr;
		    },
		    "missing face model dependency");
		ok &= run_missing_preflight(
		    [](auto &missing_dependencies) {
			    missing_dependencies.has_graphical_display = nullptr;
		    },
		    "missing display dependency");
		ok &= run_missing_preflight(
		    [](auto &missing_dependencies) {
			    missing_dependencies.open_camera = nullptr;
		    },
		    "missing camera open dependency");
		ok &= run_missing_preflight(
		    [](auto &missing_dependencies) {
			    missing_dependencies.switch_gui_user = nullptr;
		    },
		    "missing GUI user dependency");
		ok &= run_missing_preflight(
		    [](auto &missing_dependencies) {
			    missing_dependencies.initialize_gui = nullptr;
		    },
		    "missing GUI init dependency");
		ok &= run_missing_preflight(
		    [](auto &missing_dependencies) {
			    missing_dependencies.read_camera = nullptr;
		    },
		    "missing camera read dependency");
		return ok;
	}

	auto missing_dependency_callbacks_fail_closed() -> bool {
		bool ok = true;
		{
			auto context                     = make_success_context();
			auto dependencies                = test_dependencies(context);
			dependencies.load_runtime_config = nullptr;

			const int result = run_test_with_dependencies(dependencies, {"howdy-test"});

			ok &= expect(result == 1, "missing load dependency returns 1");
			ok &= expect(context.load_calls == 0, "missing load dependency skips load");
			ok &= expect(context.preview_calls == 0, "missing load dependency skips preview");
		}
		{
			auto context             = make_success_context();
			auto dependencies        = test_dependencies(context);
			dependencies.run_preview = nullptr;

			const int result = run_test_with_dependencies(dependencies, {"howdy-test"});

			ok &= expect(result == 1, "missing preview dependency returns 1");
			ok &= expect(context.load_calls == 0, "missing preview dependency skips load");
			ok &= expect(context.preview_calls == 0, "missing preview dependency skips preview");
		}
		{
			const int result = run_test_with_dependencies(
			    howdy::native::test_internal::TestDependencies{}, {"howdy-test"});

			ok &= expect(result == 1, "empty dependencies return 1");
		}
		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= invalid_runtime_config_stops_before_preview();
	ok &= face_model_failure_returns_error();
	ok &= missing_graphical_environment_prints_diagnostic();
	ok &= camera_open_failure_prints_device_and_capture_error();
	ok &= camera_read_failure_prints_diagnostic();
	ok &= gui_user_switch_failure_prints_diagnostic();
	ok &= successful_preview_returns_zero();
	ok &= configured_device_default_is_used_for_camera_open();
	ok &= device_override_is_used_for_camera_open();
	ok &= gui_initialization_runs_before_first_camera_read();
	ok &= graphical_environment_helper_checks_display_values();
	ok &= missing_preflight_dependency_callbacks_fail_closed();
	ok &= missing_dependency_callbacks_fail_closed();
	return ok ? 0 : 1;
}
