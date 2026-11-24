#include "cli/snapshot.hpp"

#include "app/command_invocation.hpp"
#include "cli/snapshot/internal.hpp"
#include "config/runtime_config.hpp"
#include "config/runtime_config_loader.hpp"
#include "config/runtime_paths.hpp"
#include "support/atomic_files.hpp"
#include "support/capture_exposure.hpp"
#include "vision/video_capture.hpp"

#include <chrono>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <opencv2/core/hal/interface.h>
#include <opencv2/core/mat.hpp>
#include <opencv2/imgcodecs.hpp>

namespace {

	namespace snapshot_internal = howdy::native::snapshot_internal;

	auto SnapshotPath(std::chrono::sys_seconds timestamp) -> std::filesystem::path {
		return howdy::native::ResolveLogPath() / "snapshots" /
		       std::format("{:%Y%m%dT%H%M%S}.jpg", timestamp);
	}

	auto EncodeImageDependency([[maybe_unused]] void *context, std::string_view extension,
	                           const cv::Mat &image, std::vector<uchar> *encoded) -> bool {
		return cv::imencode(std::string(extension), image, *encoded);
	}

	auto GenerateSnapshot(const std::vector<cv::Mat>     &frames,
	                      const std::vector<std::string> &text_lines,
	                      std::chrono::sys_seconds        timestamp) -> std::filesystem::path {
		const auto base_path = SnapshotPath(timestamp);
		if (base_path.empty()) {
			return {};
		}
		const auto dependencies = snapshot_internal::SnapshotWriterDependencies{
		    .context      = nullptr,
		    .encode_image = EncodeImageDependency,
		};
		howdy::native::AtomicFileCommitResult commit_result =
		    howdy::native::AtomicFileCommitResult::kNotCommitted;
		auto filepath = snapshot_internal::WriteSnapshotWithUniquePath(
		    frames, text_lines, base_path, dependencies, &commit_result);
		if (filepath.empty()) {
			if (howdy::native::AtomicFileMayHaveCommitted(commit_result)) {
				std::cerr << "Snapshot was written, but its directory could not be synced; verify "
				             "the file before retrying\n";
			}
			return {};
		}
		return filepath;
	}

	auto SnapshotCliLoadRuntimeConfigDependency([[maybe_unused]] void *context)
	    -> howdy::native::RuntimeConfigLoadResult {
		return howdy::native::LoadRuntimeConfig();
	}

	auto CaptureFramesDependency([[maybe_unused]] void              *context,
	                             const howdy::native::RuntimeConfig &config)
	    -> snapshot_internal::SnapshotCaptureResult {
		howdy::native::VideoCapture capture(howdy::native::LoadCaptureSettings(config.video));
		if (!howdy::native::OpenCaptureWithExposure(capture, config.video.exposure)) {
			return snapshot_internal::SnapshotCaptureResult{
			    .status        = snapshot_internal::SnapshotCaptureStatus::kOpenError,
			    .error_message = capture.ErrorMessage(),
			};
		}

		std::vector<cv::Mat> frames;
		frames.reserve(snapshot_internal::kSnapshotFrameCount);
		while (frames.size() < snapshot_internal::kSnapshotFrameCount) {
			cv::Mat frame;
			if (!capture.Read(frame)) {
				capture.Release();
				return snapshot_internal::SnapshotCaptureResult{
				    .status = snapshot_internal::SnapshotCaptureStatus::kReadError,
				};
			}
			frames.push_back(frame);
		}
		capture.Release();

		return snapshot_internal::SnapshotCaptureResult{
		    .status = snapshot_internal::SnapshotCaptureStatus::kOk,
		    .frames = std::move(frames),
		};
	}

	auto WriteSnapshotDependency([[maybe_unused]] void *context, const std::vector<cv::Mat> &frames,
	                             const howdy::native::RuntimeConfig &config)
	    -> snapshot_internal::SnapshotWriteResult {
		const auto timestamp =
		    std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());

		const auto filepath = GenerateSnapshot(
		    frames,
		    {
		        "GENERATED SNAPSHOT",
		        std::format("Date: {:%Y/%m/%d %H:%M:%S} UTC", timestamp),
		        "Dark threshold config: " + std::to_string(config.video.dark_threshold),
		        "SFace threshold config: " + std::to_string(config.face.sface_threshold),
		    },
		    timestamp);
		if (filepath.empty()) {
			return snapshot_internal::SnapshotWriteResult{};
		}
		return snapshot_internal::SnapshotWriteResult{
		    .ok   = true,
		    .path = filepath,
		};
	}

}  // namespace

auto SnapshotMain(const howdy::native::CommandInvocation &invocation) -> int {
	return howdy::native::snapshot_internal::SnapshotMainWithDependencies(
	    invocation, howdy::native::snapshot_internal::SnapshotDependencies{
	                    .load_runtime_config = SnapshotCliLoadRuntimeConfigDependency,
	                    .capture_frames      = CaptureFramesDependency,
	                    .write_snapshot      = WriteSnapshotDependency,
	                });
}
