#include "cli/snapshot.hpp"

#include "cli/snapshot/internal.hpp"
#include "config/runtime_config_loader.hpp"
#include "config/runtime_paths.hpp"
#include "support/atomic_files.hpp"
#include "vision/video_capture.hpp"

#include <array>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>

namespace {

	namespace snapshot_internal = howdy::native::snapshot_internal;

	auto SnapshotPath() -> std::filesystem::path {
		const auto now  = std::chrono::system_clock::now();
		const auto time = std::chrono::system_clock::to_time_t(now);
		std::tm    buffer{};
		gmtime_r(&time, &buffer);
		std::array<char, 32> filename{};
		std::strftime(filename.data(), filename.size(), "%Y%m%dT%H%M%S.jpg", &buffer);
		return howdy::native::ResolveLogPath() / "snapshots" / filename.data();
	}

	auto EncodeImageDependency(void *context, std::string_view extension, const cv::Mat &image,
	                           std::vector<uchar> *encoded) -> bool {
		(void)context;
		return cv::imencode(std::string(extension), image, *encoded);
	}

	auto GenerateSnapshot(const std::vector<cv::Mat>     &frames,
	                      const std::vector<std::string> &text_lines) -> std::filesystem::path {
		const auto base_path    = SnapshotPath();
		const auto dependencies = snapshot_internal::SnapshotWriterDependencies{
		    .context      = nullptr,
		    .encode_image = EncodeImageDependency,
		};
		howdy::native::AtomicFileCommitResult commit_result;
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

	auto SnapshotCliLoadRuntimeConfigDependency(void *context)
	    -> howdy::native::RuntimeConfigLoadResult {
		(void)context;
		return howdy::native::LoadRuntimeConfig();
	}

	auto CaptureFramesDependency(void *context, const howdy::native::RuntimeConfig &config)
	    -> snapshot_internal::SnapshotCaptureResult {
		(void)context;

		howdy::native::VideoCapture capture(howdy::native::LoadCaptureSettings(config.video));
		if (!capture.Open()) {
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

	auto WriteSnapshotDependency(void *context, const std::vector<cv::Mat> &frames,
	                             const howdy::native::RuntimeConfig &config)
	    -> snapshot_internal::SnapshotWriteResult {
		(void)context;

		const auto now  = std::chrono::system_clock::now();
		const auto time = std::chrono::system_clock::to_time_t(now);
		std::tm    buffer{};
		gmtime_r(&time, &buffer);
		std::array<char, 64> timestr{};
		std::strftime(timestr.data(), timestr.size(), "%Y/%m/%d %H:%M:%S UTC", &buffer);

		const auto filepath = GenerateSnapshot(
		    frames, {
		                "GENERATED SNAPSHOT",
		                std::string("Date: ") + timestr.data(),
		                "Dark threshold config: " + std::to_string(config.video.dark_threshold),
		                "SFace threshold config: " + std::to_string(config.face.sface_threshold),
		            });
		if (filepath.empty()) {
			return snapshot_internal::SnapshotWriteResult{};
		}
		return snapshot_internal::SnapshotWriteResult{
		    .ok   = true,
		    .path = filepath,
		};
	}

}  // namespace

auto SnapshotMain(int argc, char **argv) -> int {
	return howdy::native::snapshot_internal::SnapshotMainWithDependencies(
	    argc, argv,
	    howdy::native::snapshot_internal::SnapshotDependencies{
	        .load_runtime_config = SnapshotCliLoadRuntimeConfigDependency,
	        .capture_frames      = CaptureFramesDependency,
	        .write_snapshot      = WriteSnapshotDependency,
	    });
}
