#pragma once

#include "config/runtime_config.hpp"

#include <filesystem>
#include <string>
#include <vector>

#include <opencv2/core/mat.hpp>

namespace howdy::native::snapshot_internal {

	enum class SnapshotCaptureStatus {
		kOk,
		kOpenError,
		kReadError,
	};

	struct SnapshotCaptureResult {
		SnapshotCaptureStatus status = SnapshotCaptureStatus::kOpenError;
		std::string           error_message;
		std::vector<cv::Mat>  frames;
	};

	struct SnapshotWriteResult {
		bool                  ok = false;
		std::filesystem::path path;
	};

	using LoadRuntimeConfigFn = howdy::native::RuntimeConfigLoadResult (*)(void *context);
	using CaptureFramesFn = SnapshotCaptureResult (*)(void                               *context,
	                                                  const howdy::native::RuntimeConfig &config);
	using WriteSnapshotFn = SnapshotWriteResult (*)(void                               *context,
	                                                const std::vector<cv::Mat>         &frames,
	                                                const howdy::native::RuntimeConfig &config);

	struct SnapshotDependencies {
		void               *context             = nullptr;
		LoadRuntimeConfigFn load_runtime_config = nullptr;
		CaptureFramesFn     capture_frames      = nullptr;
		WriteSnapshotFn     write_snapshot      = nullptr;
	};

	auto snapshot_main_with_dependencies(int argc, char **argv,
	                                     const SnapshotDependencies &dependencies) -> int;

}  // namespace howdy::native::snapshot_internal
