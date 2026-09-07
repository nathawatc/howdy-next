#include "cli/test/internal.hpp"
#include "test_support.hpp"
#include "vision/preview_engine.hpp"

#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

	using howdy::test::expect;

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

	auto ExpectSequence(const std::vector<std::string> &actual,
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

	auto ValidConfigLoadResult() -> howdy::native::RuntimeConfigLoadResult {
		howdy::native::RuntimeConfig config;
		config.video.dark_threshold = 41.0F;
		config.video.device_path    = "/dev/video-default";
		return howdy::native::RuntimeConfigLoadResult{
		    .ok     = true,
		    .status = howdy::native::RuntimeConfigLoadStatus::kOk,
		    .config = config,
		};
	}

	auto InvalidConfigLoadResult() -> howdy::native::RuntimeConfigLoadResult {
		return howdy::native::RuntimeConfigLoadResult{
		    .ok            = false,
		    .status        = howdy::native::RuntimeConfigLoadStatus::kInvalidRuntimeValue,
		    .error_message = "invalid runtime config",
		};
	}

	auto LoadRuntimeConfigCallback(void *raw_context) -> howdy::native::RuntimeConfigLoadResult {
		auto *context = static_cast<TestCliTestContext *>(raw_context);
		++context->load_calls;
		return context->config_result;
	}

	auto FaceModelReadyCallback(void *raw_context, const howdy::native::RuntimeConfig &config,
	                            const std::string &user)
	    -> howdy::native::test_cli_internal::TestPreflightOperationResult {
		(void)config;
		auto *context = static_cast<TestCliTestContext *>(raw_context);
		++context->face_calls;
		context->sequence.emplace_back("face");
		context->face_user = user;
		if (!context->face_model_ok) {
			return howdy::native::test_cli_internal::TestPreflightOperationResult{
			    .error_message = context->face_model_error,
			};
		}
		return howdy::native::test_cli_internal::TestPreflightOperationResult{.ok = true};
	}

	auto HasGraphicalDisplayCallback(void *raw_context) -> bool {
		auto *context = static_cast<TestCliTestContext *>(raw_context);
		++context->display_calls;
		context->sequence.emplace_back("display");
		return context->graphical_display;
	}

	auto OpenCameraCallback(void *raw_context, const howdy::native::RuntimeConfig &config,
	                        const std::string &device_path)
	    -> howdy::native::test_cli_internal::TestPreflightOperationResult {
		auto *context = static_cast<TestCliTestContext *>(raw_context);
		++context->open_calls;
		context->sequence.emplace_back("open");
		context->open_config        = config;
		context->opened_device_path = device_path;
		if (!context->camera_open_ok) {
			return howdy::native::test_cli_internal::TestPreflightOperationResult{
			    .error_message = context->camera_open_error,
			};
		}
		return howdy::native::test_cli_internal::TestPreflightOperationResult{.ok = true};
	}

	auto ReadCameraCallback(void *raw_context) -> bool {
		auto *context = static_cast<TestCliTestContext *>(raw_context);
		++context->read_calls;
		context->sequence.emplace_back("read");
		return context->camera_read_ok;
	}

	auto SwitchGuiUserCallback(void *raw_context) -> bool {
		auto *context = static_cast<TestCliTestContext *>(raw_context);
		++context->gui_calls;
		context->sequence.emplace_back("switch_gui_user");
		return context->gui_user_ok;
	}

	void InitializeGuiCallback(void *raw_context) {
		auto *context = static_cast<TestCliTestContext *>(raw_context);
		++context->gui_init_calls;
		context->sequence.emplace_back("initialize_gui");
	}

	auto PreflightDependencies(TestCliTestContext &context)
	    -> howdy::native::test_cli_internal::TestPreviewPreflightDependencies {
		return howdy::native::test_cli_internal::TestPreviewPreflightDependencies{
		    .context               = &context,
		    .face_model_ready      = FaceModelReadyCallback,
		    .has_graphical_display = HasGraphicalDisplayCallback,
		    .open_camera           = OpenCameraCallback,
		    .read_camera           = ReadCameraCallback,
		    .switch_gui_user       = SwitchGuiUserCallback,
		    .initialize_gui        = InitializeGuiCallback,
		};
	}

	auto RunPreviewCallback(void *raw_context, const howdy::native::RuntimeConfig &config,
	                        const std::string &user, const std::string &device_path)
	    -> howdy::native::test_cli_internal::TestPreviewResult {
		auto *context = static_cast<TestCliTestContext *>(raw_context);
		++context->preview_calls;
		context->preview_config      = config;
		context->preview_user        = user;
		context->preview_device_path = device_path;
		return howdy::native::test_cli_internal::RunPreviewPreflight(
		    config, user, PreflightDependencies(*context), device_path);
	}

	auto TestDependencies(TestCliTestContext &context)
	    -> howdy::native::test_cli_internal::TestDependencies {
		return howdy::native::test_cli_internal::TestDependencies{
		    .context             = &context,
		    .load_runtime_config = LoadRuntimeConfigCallback,
		    .run_preview         = RunPreviewCallback,
		};
	}

	auto RunTestWithDependencies(howdy::native::test_cli_internal::TestDependencies dependencies,
	                             std::vector<std::string> arguments) -> int {
		std::vector<char *> argv;
		argv.reserve(arguments.size());
		for (auto &argument : arguments) {
			argv.push_back(argument.data());
		}
		return howdy::native::test_cli_internal::TestMainWithDependencies(
		    static_cast<int>(argv.size()), argv.data(), dependencies);
	}

	auto RunTest(TestCliTestContext &context, std::vector<std::string> arguments) -> int {
		return RunTestWithDependencies(TestDependencies(context), std::move(arguments));
	}

	auto MakeSuccessContext() -> TestCliTestContext {
		TestCliTestContext context;
		context.config_result = ValidConfigLoadResult();
		return context;
	}

	auto InvalidRuntimeConfigStopsBeforePreview() -> bool {
		auto context          = MakeSuccessContext();
		context.config_result = InvalidConfigLoadResult();
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = RunTest(context, {"howdy-test"});

		bool ok = true;
		ok &= expect(result == 1, "invalid runtime config returns 1");
		ok &= expect(context.load_calls == 1, "invalid runtime config loads once");
		ok &= expect(context.preview_calls == 0, "invalid runtime config skips preview");
		ok &= expect(context.face_calls == 0, "invalid runtime config skips face preflight");
		ok &= expect(error.str().contains("invalid runtime config"),
		             "invalid runtime config writes config error");
		return ok;
	}

	auto FaceModelFailureReturnsError() -> bool {
		auto context             = MakeSuccessContext();
		context.face_model_ok    = false;
		context.face_model_error = "face model failed";
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = RunTest(context, {"howdy-test", "alice"});

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

	auto PreviewFrameFailuresMapToCliStopStatus() -> bool {
		const auto invalid_frame = howdy::native::test_cli_internal::MapPreviewFrameFailure({
		    .status        = howdy::native::PreviewFrameStatus::kInvalidFrame,
		    .error_message = "invalid camera frame",
		});
		bool ok = expect(invalid_frame.status ==
		                     howdy::native::test_cli_internal::TestPreviewStatus::kCameraReadError,
		                 "invalid preview frame maps to camera read error");
		ok &= expect(invalid_frame.error_message == "invalid camera frame",
		             "invalid preview frame preserves diagnostic");

		auto expect_face_model_error = [](howdy::native::PreviewFrameStatus status,
		                                  const std::string                &diagnostic,
		                                  const std::string                &subject) -> bool {
			const auto result = howdy::native::test_cli_internal::MapPreviewFrameFailure({
			    .status        = status,
			    .error_message = diagnostic,
			});
			return expect(result.status ==
			                  howdy::native::test_cli_internal::TestPreviewStatus::kFaceModelError,
			              subject + " maps to face model error") &&
			       expect(result.error_message == diagnostic, subject + " preserves diagnostic");
		};

		ok &= expect_face_model_error(howdy::native::PreviewFrameStatus::kDetectionFailed,
		                              "detector failed", "preview preprocessing/detection failure");
		ok &= expect_face_model_error(howdy::native::PreviewFrameStatus::kEncodingFailed,
		                              "first encoder failed", "all preview encodings failed");
		ok &= expect_face_model_error(howdy::native::PreviewFrameStatus::kInvalidMatchResult,
		                              "matcher returned invalid model index",
		                              "invalid preview match result");
		ok &= expect_face_model_error(howdy::native::PreviewFrameStatus::kInvalidDependencies,
		                              "preview dependency missing", "invalid preview dependencies");
		return ok;
	}

	auto PreviewNonTerminalFramesMapToOk() -> bool {
		auto expect_continue = [](howdy::native::PreviewFrameStatus status,
		                          const std::string                &subject) -> bool {
			const auto result = howdy::native::test_cli_internal::MapPreviewFrameFailure({
			    .status = status,
			});
			return expect(result.status == howdy::native::test_cli_internal::TestPreviewStatus::kOk,
			              subject + " keeps preview running");
		};

		bool ok = true;
		ok &= expect_continue(howdy::native::PreviewFrameStatus::kBlackFrame, "black frame");
		ok &= expect_continue(howdy::native::PreviewFrameStatus::kTooDark, "too-dark frame");
		ok &= expect_continue(howdy::native::PreviewFrameStatus::kNoFace, "no-face frame");
		ok &= expect_continue(howdy::native::PreviewFrameStatus::kFacesDetected,
		                      "detected-only frame");
		ok &= expect_continue(howdy::native::PreviewFrameStatus::kUnmatchedFace,
		                      "unmatched-face frame");
		ok &=
		    expect_continue(howdy::native::PreviewFrameStatus::kMatchedFace, "matched-face frame");
		return ok;
	}

	auto MissingGraphicalEnvironmentPrintsDiagnostic() -> bool {
		auto context              = MakeSuccessContext();
		context.graphical_display = false;
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = RunTest(context, {"howdy-test"});

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
		ok &= expect(error_text.contains("automatically detects a standard Wayland session"),
		             "missing graphical environment explains automatic Wayland detection");
		ok &= expect(error_text.contains("run0 --setenv=WAYLAND_DISPLAY howdy test"),
		             "missing graphical environment writes Wayland run0 fallback");
		ok &= expect(error_text.contains("sudo --preserve-env=DISPLAY,XAUTHORITY"),
		             "missing graphical environment writes X11 sudo fallback");
		ok &= expect(error_text.contains("sudo howdy snapshot"),
		             "missing graphical environment writes headless fallback");
		return ok;
	}

	auto CameraOpenFailurePrintsDeviceAndCaptureError() -> bool {
		auto context              = MakeSuccessContext();
		context.camera_open_ok    = false;
		context.camera_open_error = "camera open failed";
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = RunTest(context, {"howdy-test"});

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

	auto CameraReadFailurePrintsDiagnostic() -> bool {
		auto context           = MakeSuccessContext();
		context.camera_read_ok = false;
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = RunTest(context, {"howdy-test"});

		bool ok = true;
		ok &= expect(result == 1, "camera read failure returns 1");
		ok &= expect(context.preview_calls == 1, "camera read failure runs preview once");
		ok &= expect(context.open_calls == 1, "camera read failure opens camera once");
		ok &= expect(context.gui_calls == 1, "camera read failure switches GUI user once");
		ok &= expect(context.gui_init_calls == 1, "camera read failure initializes GUI once");
		ok &= expect(context.read_calls == 1, "camera read failure reads camera once");
		ok &= expect(error.str().contains("Could not capture a camera frame"),
		             "camera read failure writes diagnostic");
		return ok;
	}

	auto GuiUserSwitchFailurePrintsDiagnostic() -> bool {
		auto context        = MakeSuccessContext();
		context.gui_user_ok = false;
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = RunTest(context, {"howdy-test"});

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

	auto SuccessfulPreviewReturnsZero() -> bool {
		auto               context = MakeSuccessContext();
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = RunTest(context, {"howdy-test", "alice"});

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

	auto ConfiguredDeviceDefaultIsUsedForCameraOpen() -> bool {
		auto               context = MakeSuccessContext();
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = RunTest(context, {"howdy-test"});

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

	auto UnconfiguredCameraErrorIsPrinted() -> bool {
		auto context = MakeSuccessContext();
		if (!context.config_result.config.has_value()) {
			return expect(false, "success context has valid config");
		}
		context.config_result.config->video.device_path = "none";
		context.camera_open_ok                          = false;
		context.camera_open_error = "Camera is not configured; set video.device_path";
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int  result     = RunTest(context, {"howdy-test"});
		const auto error_text = error.str();

		bool ok = true;
		ok &= expect(result == 1, "unconfigured camera returns 1");
		ok &= expect(context.opened_device_path == "none",
		             "unconfigured camera passes default sentinel to open");
		ok &= expect(error_text == context.camera_open_error + "\n",
		             "unconfigured camera prints only concise actionable error");
		return ok;
	}

	auto DeviceOverrideIsUsedForCameraOpen() -> bool {
		auto               context = MakeSuccessContext();
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = RunTest(context, {"howdy-test", "--device", "/dev/video1"});

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

	auto MissingDeviceValueIsRejectedBeforeRuntimeWork() -> bool {
		auto               context = MakeSuccessContext();
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = RunTest(context, {"howdy-test", "--device"});

		bool ok = true;
		ok &= expect(result == 1, "missing device value returns 1");
		ok &= expect(error.str().contains("--device requires a non-empty value"),
		             "missing device value writes diagnostic");
		ok &= expect(context.load_calls == 0, "missing device value skips config load");
		ok &= expect(context.preview_calls == 0, "missing device value skips preview");
		return ok;
	}

	auto InvalidDeviceOptionsStopBeforeRuntimeWork() -> bool {
		bool ok = true;
		for (const auto &arguments : std::vector<std::vector<std::string>>{
		         {"howdy-test", "--device", ""},
		         {"howdy-test", "--device", "--unknown"},
		         {"howdy-test", "--unknown"},
		         {"howdy-test", "--device", "/dev/video0", "--device", "/dev/video1"},
		     }) {
			auto               context = MakeSuccessContext();
			std::ostringstream error;
			StreamRedirect     error_redirect(std::cerr, error.rdbuf());

			const int result = RunTest(context, arguments);

			ok &= expect(result == 1, "invalid device option returns 1");
			ok &= expect(context.load_calls == 0 && context.preview_calls == 0,
			             "invalid device option skips runtime work");
			ok &= expect(error.str().contains("device") || error.str().contains("invalid"),
			             "invalid device option writes diagnostic");
		}
		return ok;
	}

	auto GuiInitializationRunsBeforeFirstCameraRead() -> bool {
		auto               context = MakeSuccessContext();
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = RunTest(context, {"howdy-test"});

		bool ok = true;
		ok &= expect(result == 0, "GUI/read sequence returns 0");
		ok &=
		    ExpectSequence(context.sequence,
		                   {"face", "display", "open", "switch_gui_user", "initialize_gui", "read"},
		                   "GUI initialization runs before first camera read");
		ok &= expect(error.str().empty(), "GUI/read sequence writes no failure diagnostic");
		return ok;
	}

	auto GraphicalEnvironmentHelperChecksDisplayValues() -> bool {
		namespace test_cli_internal = howdy::native::test_cli_internal;

		bool ok = true;
		ok &= expect(!test_cli_internal::HasGraphicalDisplayEnvironment("", "", ""),
		             "empty display values are not graphical");
		ok &= expect(test_cli_internal::HasGraphicalDisplayEnvironment(":0", "", ""),
		             "DISPLAY enables graphical environment");
		ok &= expect(!test_cli_internal::HasGraphicalDisplayEnvironment("", "wayland-0", ""),
		             "Wayland display without runtime dir is not graphical");
		ok &= expect(
		    test_cli_internal::HasGraphicalDisplayEnvironment("", "wayland-0", "/run/user/1000"),
		    "Wayland display with runtime dir is graphical");
		ok &= expect(!test_cli_internal::HasGraphicalDisplayEnvironment("", "", "/run/user/1000"),
		             "empty DISPLAY does not count as graphical");
		return ok;
	}

	auto MissingPreflightDependencyCallbacksFailClosed() -> bool {
		auto context = MakeSuccessContext();
		if (!context.config_result.config.has_value()) {
			return expect(false, "success context has valid config");
		}
		auto dependencies = PreflightDependencies(context);

		auto run_missing_preflight = [&](auto clear_callback, const std::string &message) -> bool {
			auto missing_dependencies = dependencies;
			clear_callback(missing_dependencies);
			const auto result = howdy::native::test_cli_internal::RunPreviewPreflight(
			    *context.config_result.config, "", missing_dependencies, "");
			return expect(result.status ==
			                  howdy::native::test_cli_internal::TestPreviewStatus::kFaceModelError,
			              message + " returns face model error") &&
			       expect(
			           result.error_message.contains("missing test preview preflight dependency"),
			           message + " writes internal error") &&
			       expect(context.sequence.empty(), message + " skips all preflight operations");
		};

		bool ok = true;
		ok &= run_missing_preflight(
		    [](auto &missing_dependencies) -> auto {
			    missing_dependencies.face_model_ready = nullptr;
		    },
		    "missing face model dependency");
		ok &= run_missing_preflight(
		    [](auto &missing_dependencies) -> auto {
			    missing_dependencies.has_graphical_display = nullptr;
		    },
		    "missing display dependency");
		ok &= run_missing_preflight(
		    [](auto &missing_dependencies) -> auto {
			    missing_dependencies.open_camera = nullptr;
		    },
		    "missing camera open dependency");
		ok &= run_missing_preflight(
		    [](auto &missing_dependencies) -> auto {
			    missing_dependencies.switch_gui_user = nullptr;
		    },
		    "missing GUI user dependency");
		ok &= run_missing_preflight(
		    [](auto &missing_dependencies) -> auto {
			    missing_dependencies.initialize_gui = nullptr;
		    },
		    "missing GUI init dependency");
		ok &= run_missing_preflight(
		    [](auto &missing_dependencies) -> auto {
			    missing_dependencies.read_camera = nullptr;
		    },
		    "missing camera read dependency");
		return ok;
	}

	auto MissingDependencyCallbacksFailClosed() -> bool {
		bool ok = true;
		{
			auto context                     = MakeSuccessContext();
			auto dependencies                = TestDependencies(context);
			dependencies.load_runtime_config = nullptr;

			const int result = RunTestWithDependencies(dependencies, {"howdy-test"});

			ok &= expect(result == 1, "missing load dependency returns 1");
			ok &= expect(context.load_calls == 0, "missing load dependency skips load");
			ok &= expect(context.preview_calls == 0, "missing load dependency skips preview");
		}
		{
			auto context             = MakeSuccessContext();
			auto dependencies        = TestDependencies(context);
			dependencies.run_preview = nullptr;

			const int result = RunTestWithDependencies(dependencies, {"howdy-test"});

			ok &= expect(result == 1, "missing preview dependency returns 1");
			ok &= expect(context.load_calls == 0, "missing preview dependency skips load");
			ok &= expect(context.preview_calls == 0, "missing preview dependency skips preview");
		}
		{
			const int result = RunTestWithDependencies(
			    howdy::native::test_cli_internal::TestDependencies{}, {"howdy-test"});

			ok &= expect(result == 1, "empty dependencies return 1");
		}
		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= InvalidRuntimeConfigStopsBeforePreview();
	ok &= FaceModelFailureReturnsError();
	ok &= PreviewFrameFailuresMapToCliStopStatus();
	ok &= PreviewNonTerminalFramesMapToOk();
	ok &= MissingGraphicalEnvironmentPrintsDiagnostic();
	ok &= CameraOpenFailurePrintsDeviceAndCaptureError();
	ok &= CameraReadFailurePrintsDiagnostic();
	ok &= GuiUserSwitchFailurePrintsDiagnostic();
	ok &= SuccessfulPreviewReturnsZero();
	ok &= ConfiguredDeviceDefaultIsUsedForCameraOpen();
	ok &= UnconfiguredCameraErrorIsPrinted();
	ok &= DeviceOverrideIsUsedForCameraOpen();
	ok &= MissingDeviceValueIsRejectedBeforeRuntimeWork();
	ok &= InvalidDeviceOptionsStopBeforeRuntimeWork();
	ok &= GuiInitializationRunsBeforeFirstCameraRead();
	ok &= GraphicalEnvironmentHelperChecksDisplayValues();
	ok &= MissingPreflightDependencyCallbacksFailClosed();
	ok &= MissingDependencyCallbacksFailClosed();
	return ok ? 0 : 1;
}
