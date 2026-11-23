#include "app/command_invocation.hpp"
#include "cli/test/internal.hpp"
#include "config/runtime_config.hpp"
#include "config/runtime_config_loader.hpp"
#include "support/capture_device_path.hpp"
#include "support/invoking_user_env.hpp"
#include "vision/video_capture.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

	constexpr int kWorkflowExitOk          = 0;
	constexpr int kWorkflowExitCameraError = 1;

	void PrintMissingGraphicalEnvironmentDiagnostic() {
		std::cerr << "Cannot open the interactive test preview because no graphical display "
		             "environment is available.\n";
		std::cerr << "Howdy automatically detects a standard Wayland session for the invoking "
		             "user.\n\n";
		std::cerr
		    << "If multiple Wayland displays are active, pass the intended display explicitly:\n";
		std::cerr << "  run0 --setenv=" << howdy::native::kWaylandDisplayEnvironmentVariable
		          << " howdy test\n";
		std::cerr << "  sudo --preserve-env=" << howdy::native::kWaylandDisplayEnvironmentVariable
		          << " howdy test\n\n";
		std::cerr << "For X11, preserve its display credentials instead:\n";
		std::cerr << "  run0 --setenv=DISPLAY --setenv=XAUTHORITY howdy test\n";
		std::cerr << "  sudo --preserve-env=DISPLAY,XAUTHORITY howdy test\n\n";
		std::cerr << "For headless testing, use:\n";
		std::cerr << "  sudo howdy snapshot\n";
	}

}  // namespace

auto howdy::native::test_cli_internal::RunPreviewPreflight(
    const howdy::native::RuntimeConfig &config, const std::string &user,
    const TestPreviewPreflightDependencies &dependencies, const std::string &device_path)
    -> TestPreviewResult {
	if (dependencies.face_model_ready == nullptr || dependencies.has_graphical_display == nullptr ||
	    dependencies.open_camera == nullptr || dependencies.read_camera == nullptr ||
	    dependencies.switch_gui_user == nullptr || dependencies.initialize_gui == nullptr) {
		return TestPreviewResult{
		    .status        = TestPreviewStatus::kFaceModelError,
		    .error_message = "Internal error: missing test preview preflight dependency",
		};
	}

	auto const face_model_result =
	    dependencies.face_model_ready(dependencies.context, config, user);
	if (!face_model_result.ok) {
		return TestPreviewResult{
		    .status        = TestPreviewStatus::kFaceModelError,
		    .error_message = face_model_result.error_message,
		};
	}

	auto settings = howdy::native::LoadCaptureSettings(config.video);
	if (!device_path.empty()) {
		settings.device_path = device_path;
	}

	if (!dependencies.has_graphical_display(dependencies.context)) {
		return TestPreviewResult{.status = TestPreviewStatus::kMissingGraphicalEnvironment};
	}

	auto const open_result =
	    dependencies.open_camera(dependencies.context, config, settings.device_path);
	if (!open_result.ok) {
		return TestPreviewResult{
		    .status        = TestPreviewStatus::kCameraOpenError,
		    .error_message = open_result.error_message,
		    .device_path   = settings.device_path,
		};
	}

	std::cout << "\nOpening a window with a test feed\n\n";
	std::cout << "Press Ctrl+C here or Q in preview window to quit\n";
	std::cout << "Click on the image to enable or disable slow mode\n\n";

	if (!dependencies.switch_gui_user(dependencies.context)) {
		return TestPreviewResult{.status = TestPreviewStatus::kGuiUserError};
	}

	dependencies.initialize_gui(dependencies.context);

	if (!dependencies.read_camera(dependencies.context)) {
		return TestPreviewResult{.status = TestPreviewStatus::kCameraReadError};
	}

	return TestPreviewResult{.status = TestPreviewStatus::kOk};
}

auto howdy::native::test_cli_internal::HasGraphicalDisplayEnvironment(
    std::string_view display, std::string_view wayland_display, std::string_view runtime_dir)
    -> bool {
	if (!display.empty()) {
		return true;
	}
	return !wayland_display.empty() && !runtime_dir.empty();
}

auto howdy::native::test_cli_internal::TestMainWithDependencies(
    const howdy::native::CommandInvocation &invocation, const TestDependencies &dependencies)
    -> int {
	if (dependencies.load_runtime_config == nullptr || dependencies.run_preview == nullptr) {
		return kWorkflowExitCameraError;
	}

	auto config_result = dependencies.load_runtime_config(dependencies.context);
	if (config_result.status != howdy::native::RuntimeConfigLoadStatus::kOk ||
	    !config_result.config.has_value()) {
		std::cerr << config_result.error_message << "\n";
		return kWorkflowExitCameraError;
	}
	const auto &config = *config_result.config;

	const auto preview_result = dependencies.run_preview(dependencies.context, config,
	                                                     invocation.resolved_user.value_or(""),
	                                                     invocation.device.value_or(""));
	switch (preview_result.status) {
		case TestPreviewStatus::kOk:
			return kWorkflowExitOk;
		case TestPreviewStatus::kFaceModelError:
			std::cerr << preview_result.error_message << "\n";
			return kWorkflowExitCameraError;
		case TestPreviewStatus::kMissingGraphicalEnvironment:
			PrintMissingGraphicalEnvironmentDiagnostic();
			return kWorkflowExitCameraError;
		case TestPreviewStatus::kCameraOpenError:
			if (preview_result.device_path == howdy::native::kNoCaptureDevice) {
				std::cerr << preview_result.error_message << "\n";
			} else {
				std::cerr << "Failed to open camera device: " << preview_result.device_path << "\n";
				std::cerr << "Error: " << preview_result.error_message << "\n";
			}
			return kWorkflowExitCameraError;
		case TestPreviewStatus::kCameraReadError:
			std::cerr << howdy::native::kCameraReadFailureMessage << '\n';
			return kWorkflowExitCameraError;
		case TestPreviewStatus::kGuiUserError:
			std::cerr << "Failed to switch GUI session to the invoking user\n";
			// A specific credential error means the invocation was fine and the advice
			// below would mislead.
			if (!preview_result.error_message.empty()) {
				std::cerr << "Error: " << preview_result.error_message << "\n";
			} else {
				std::cerr
				    << "Run this command from your desktop session through sudo/doas/pkexec\n";
			}
			return kWorkflowExitCameraError;
		default:
			std::cerr << "Internal error: unknown test preview status\n";
			return kWorkflowExitCameraError;
	}
}
