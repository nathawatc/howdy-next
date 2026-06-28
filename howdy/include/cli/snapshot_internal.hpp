#pragma once

#include "config/runtime_config.hpp"

#include <filesystem>
#include <string>
#include <vector>

#include <opencv2/core/mat.hpp>

#include <sys/stat.h>

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

	using SnapshotWriteImageFn = bool (*)(void *context, const std::filesystem::path &path,
	                                      const cv::Mat &image);
	using SnapshotChmodPathFn  = int (*)(void *context, const std::filesystem::path &path,
	                                     mode_t mode);
	using SnapshotSyncParentFn = void (*)(void *context, const std::filesystem::path &path);
	// Callback consumes fd on every outcome, including error return or exception.
	using SnapshotCloseFdFn = int (*)(void *context, int fd);

	struct SnapshotWriterDependencies {
		void                *context       = nullptr;
		SnapshotWriteImageFn write_image   = nullptr;
		SnapshotChmodPathFn  chmod_path    = nullptr;
		SnapshotSyncParentFn sync_parent   = nullptr;
		SnapshotCloseFdFn    close_temp_fd = nullptr;
	};

	auto ensure_snapshot_directory(const std::filesystem::path &directory) -> bool;

	auto write_snapshot_at_path(const std::vector<cv::Mat>       &frames,
	                            const std::vector<std::string>   &text_lines,
	                            const std::filesystem::path      &path,
	                            const SnapshotWriterDependencies &dependencies) -> bool;

	auto snapshot_main_with_dependencies(int argc, char **argv,
	                                     const SnapshotDependencies &dependencies) -> int;

}  // namespace howdy::native::snapshot_internal
