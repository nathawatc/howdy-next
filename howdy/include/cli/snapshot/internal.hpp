#pragma once

#include "config/runtime_config.hpp"
#include "support/atomic_files.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <opencv2/core.hpp>

namespace howdy::native::snapshot_internal {

	inline constexpr std::size_t kSnapshotFrameCount      = 4;
	inline constexpr std::size_t kMaxSnapshotNameAttempts = 100;

	enum class SnapshotCaptureStatus : std::uint8_t {
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

	using SnapshotEncodeImageFn = bool (*)(void *context, std::string_view extension,
	                                       const cv::Mat &image, std::vector<uchar> *encoded);

	struct SnapshotWriterDependencies {
		void                 *context      = nullptr;
		SnapshotEncodeImageFn encode_image = nullptr;
		SyncParentDirectoryFn sync_parent  = sync_parent_directory;
	};

	auto ensure_snapshot_directory(const std::filesystem::path &directory) -> bool;

	auto write_snapshot_at_path(const std::vector<cv::Mat>       &frames,
	                            const std::vector<std::string>   &text_lines,
	                            const std::filesystem::path      &path,
	                            const SnapshotWriterDependencies &dependencies,
	                            AtomicFileCommitResult           *commit_result = nullptr) -> bool;

	auto write_snapshot_with_unique_path(const std::vector<cv::Mat>       &frames,
	                                     const std::vector<std::string>   &text_lines,
	                                     const std::filesystem::path      &base_path,
	                                     const SnapshotWriterDependencies &dependencies,
	                                     AtomicFileCommitResult           *commit_result = nullptr)
	    -> std::filesystem::path;

	auto snapshot_main_with_dependencies(int argc, char **argv,
	                                     const SnapshotDependencies &dependencies) -> int;

}  // namespace howdy::native::snapshot_internal
