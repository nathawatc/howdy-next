#pragma once

#include "config/runtime_config.hpp"

#include <string>
#include <string_view>

namespace howdy::native::test_internal {

	enum class TestPreviewStatus {
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

	auto test_main_with_dependencies(int argc, char **argv, const TestDependencies &dependencies)
	    -> int;

	auto run_preview_preflight(const howdy::native::RuntimeConfig &config, const std::string &user,
	                           const std::string                      &device_path,
	                           const TestPreviewPreflightDependencies &dependencies)
	    -> TestPreviewResult;

	auto has_graphical_display_environment(std::string_view display,
	                                       std::string_view wayland_display,
	                                       std::string_view runtime_dir) -> bool;

}  // namespace howdy::native::test_internal
