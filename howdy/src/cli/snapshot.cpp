#include "cli/snapshot.hpp"

#include "cli/snapshot/internal.hpp"
#include "config/runtime_config.hpp"
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

	auto snapshot_path() -> std::filesystem::path {
		const auto now  = std::chrono::system_clock::now();
		const auto time = std::chrono::system_clock::to_time_t(now);
		std::tm    buffer{};
		gmtime_r(&time, &buffer);
		std::array<char, 32> filename{};
		std::strftime(filename.data(), filename.size(), "%Y%m%dT%H%M%S.jpg", &buffer);
		return howdy::native::resolve_log_path() / "snapshots" / filename.data();
	}

	auto encode_image_dependency(void *context, std::string_view extension, const cv::Mat &image,
	                             std::vector<uchar> *encoded) -> bool {
		(void)context;
		return cv::imencode(std::string(extension), image, *encoded);
	}

	auto generate_snapshot(const std::vector<cv::Mat>     &frames,
	                       const std::vector<std::string> &text_lines) -> std::filesystem::path {
		const auto base_path    = snapshot_path();
		const auto dependencies = snapshot_internal::SnapshotWriterDependencies{
		    .context      = nullptr,
		    .encode_image = encode_image_dependency,
		};
		howdy::native::AtomicFileCommitResult commit_result;
		auto filepath = snapshot_internal::write_snapshot_with_unique_path(
		    frames, text_lines, base_path, dependencies, &commit_result);
		if (filepath.empty()) {
			if (howdy::native::atomic_file_may_have_committed(commit_result)) {
				std::cerr << "Snapshot was written, but its directory could not be synced; verify "
				             "the file before retrying\n";
			}
			return {};
		}
		return filepath;
	}

	auto load_runtime_config_dependency(void *context) -> howdy::native::RuntimeConfigLoadResult {
		(void)context;
		return howdy::native::load_runtime_config();
	}

	auto capture_frames_dependency(void *context, const howdy::native::RuntimeConfig &config)
	    -> snapshot_internal::SnapshotCaptureResult {
		(void)context;

		howdy::native::VideoCapture capture(howdy::native::load_capture_settings(config.video));
		if (!capture.open()) {
			return snapshot_internal::SnapshotCaptureResult{
			    .status        = snapshot_internal::SnapshotCaptureStatus::kOpenError,
			    .error_message = capture.error_message(),
			};
		}

		std::vector<cv::Mat> frames;
		frames.reserve(snapshot_internal::kSnapshotFrameCount);
		while (frames.size() < snapshot_internal::kSnapshotFrameCount) {
			cv::Mat frame;
			if (!capture.read(frame)) {
				capture.release();
				return snapshot_internal::SnapshotCaptureResult{
				    .status = snapshot_internal::SnapshotCaptureStatus::kReadError,
				};
			}
			frames.push_back(frame);
		}
		capture.release();

		return snapshot_internal::SnapshotCaptureResult{
		    .status = snapshot_internal::SnapshotCaptureStatus::kOk,
		    .frames = std::move(frames),
		};
	}

	auto write_snapshot_dependency(void *context, const std::vector<cv::Mat> &frames,
	                               const howdy::native::RuntimeConfig &config)
	    -> snapshot_internal::SnapshotWriteResult {
		(void)context;

		const auto now  = std::chrono::system_clock::now();
		const auto time = std::chrono::system_clock::to_time_t(now);
		std::tm    buffer{};
		gmtime_r(&time, &buffer);
		std::array<char, 64> timestr{};
		std::strftime(timestr.data(), timestr.size(), "%Y/%m/%d %H:%M:%S UTC", &buffer);

		const auto filepath = generate_snapshot(
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

auto snapshot_main(int argc, char **argv) -> int {
	return howdy::native::snapshot_internal::snapshot_main_with_dependencies(
	    argc, argv,
	    howdy::native::snapshot_internal::SnapshotDependencies{
	        .load_runtime_config = load_runtime_config_dependency,
	        .capture_frames      = capture_frames_dependency,
	        .write_snapshot      = write_snapshot_dependency,
	    });
}
