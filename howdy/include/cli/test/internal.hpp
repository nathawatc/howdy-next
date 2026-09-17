#pragma once

#include "app/command_invocation.hpp"
#include "config/runtime_config_loader.hpp"

#include <cstdint>
#include <string>
#include <string_view>

#include <opencv2/core.hpp>

namespace howdy::native {
	struct PreviewFrameResult;
}

namespace howdy::native::test_cli_internal {
	class TestPreviewRenderer;

	enum class TestPreviewStatus : std::uint8_t {
		kOk,
		kFaceModelError,
		kMissingGraphicalEnvironment,
		kCameraOpenError,
		kCameraReadError,
		kGuiUserError,
	};

	struct TestPreviewResult {
		TestPreviewStatus status = TestPreviewStatus::kFaceModelError;
		std::string       error_message;
		std::string       device_path;
	};

	struct TestPreflightOperationResult {
		bool        ok = false;
		std::string error_message;
	};

	using LoadRuntimeConfigFn = howdy::native::RuntimeConfigLoadResult (*)(void *context);
	using FaceModelReadyFn    = TestPreflightOperationResult (*)(
	    void *context, const howdy::native::RuntimeConfig &config, const std::string &user);
	using HasGraphicalDisplayFn = bool (*)(void *context);
	using OpenCameraFn          = TestPreflightOperationResult (*)(
	    void *context, const howdy::native::RuntimeConfig &config, const std::string &device_path);
	using ReadCameraFn    = bool (*)(void *context);
	using SwitchGuiUserFn = bool (*)(void *context);
	using InitializeGuiFn = void (*)(void *context);

	struct TestPreviewPreflightDependencies {
		void                 *context               = nullptr;
		FaceModelReadyFn      face_model_ready      = nullptr;
		HasGraphicalDisplayFn has_graphical_display = nullptr;
		OpenCameraFn          open_camera           = nullptr;
		ReadCameraFn          read_camera           = nullptr;
		SwitchGuiUserFn       switch_gui_user       = nullptr;
		InitializeGuiFn       initialize_gui        = nullptr;
	};

	using RunPreviewFn = TestPreviewResult (*)(void                               *context,
	                                           const howdy::native::RuntimeConfig &config,
	                                           const std::string                  &user,
	                                           const std::string                  &device_path);

	struct TestDependencies {
		void               *context             = nullptr;
		LoadRuntimeConfigFn load_runtime_config = nullptr;
		RunPreviewFn        run_preview         = nullptr;
	};

	auto TestMainWithDependencies(const CommandInvocation &invocation,
	                              const TestDependencies  &dependencies) -> int;
	auto MapPreviewFrameFailure(const howdy::native::PreviewFrameResult &result)
	    -> TestPreviewResult;
	auto RunPreviewPreflight(const howdy::native::RuntimeConfig &config, const std::string &user,
	                         const TestPreviewPreflightDependencies &dependencies,
	                         const std::string &device_path) -> TestPreviewResult;

	auto HasGraphicalDisplayEnvironment(std::string_view display, std::string_view wayland_display,
	                                    std::string_view runtime_dir) -> bool;

}  // namespace howdy::native::test_cli_internal
