#include "cli/snapshot_internal.hpp"
#include "support/atomic_files.hpp"
#include "support/file_security.hpp"
#include "vision/frame_validation.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <opencv2/imgproc.hpp>

#include <sys/stat.h>

namespace {

	constexpr int    kExitOk                = 0;
	constexpr int    kExitAbort             = 1;
	constexpr mode_t kSnapshotDirectoryMode = S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP;
	constexpr mode_t kSnapshotFileMode      = S_IRUSR | S_IWUSR;

	[[nodiscard]] auto has_valid_snapshot_frames(const std::vector<cv::Mat> &frames) -> bool {
		if (frames.empty()) {
			return false;
		}

		const auto expected_rows  = frames.front().rows;
		const auto expected_type  = frames.front().type();
		int        combined_width = 0;

		for (const auto &frame : frames) {
			if (howdy::native::validate_frame(frame, howdy::native::FrameChannelPolicy::kBgr) !=
			    howdy::native::FrameValidationStatus::kValid) {
				return false;
			}
			if (frame.rows != expected_rows || frame.type() != expected_type) {
				return false;
			}
			if (frame.cols > howdy::native::kMaxFrameDimension - combined_width) {
				return false;
			}
			combined_width += frame.cols;
		}
		return true;
	}

	[[nodiscard]] auto snapshot_image_extension(const std::filesystem::path &path) -> std::string {
		const auto filename     = path.filename().string();
		const auto suffix_start = filename.rfind('.');
		return suffix_start == std::string::npos ? std::string{} : filename.substr(suffix_start);
	}

}  // namespace

auto howdy::native::snapshot_internal::ensure_snapshot_directory(
    const std::filesystem::path &directory) -> bool {
	const auto log_root = directory.parent_path();
	if (std::filesystem::exists(log_root)) {
		const auto root_security =
		    howdy::native::check_secure_root_owned_directory_tree(log_root, "Log directory");
		if (!root_security.ok) {
			std::cerr << root_security.error_message << "\n";
			return false;
		}
	}

	std::error_code ec;
	std::filesystem::create_directories(directory, ec);
	if (ec) {
		std::cerr << "Failed to create snapshot directory: " << directory << "\n";
		return false;
	}
	if (chmod(log_root.c_str(), kSnapshotDirectoryMode) != 0 ||
	    chmod(directory.c_str(), kSnapshotDirectoryMode) != 0) {
		std::cerr << "Failed to secure snapshot directory: " << directory << "\n";
		return false;
	}

	const auto root_security =
	    howdy::native::check_secure_root_owned_directory_tree(log_root, "Log directory");
	if (!root_security.ok) {
		std::cerr << root_security.error_message << "\n";
		return false;
	}

	const auto directory_security =
	    howdy::native::check_secure_root_owned_directory_tree(directory, "Snapshot directory");
	if (!directory_security.ok) {
		std::cerr << directory_security.error_message << "\n";
		return false;
	}
	return true;
}

auto howdy::native::snapshot_internal::write_snapshot_at_path(
    const std::vector<cv::Mat> &frames, const std::vector<std::string> &text_lines,
    const std::filesystem::path &path, const SnapshotWriterDependencies &dependencies,
    AtomicFileCommitResult *commit_result) -> bool {
	if (commit_result != nullptr) {
		*commit_result = AtomicFileCommitResult::kNotCommitted;
	}
	if (dependencies.encode_image == nullptr || dependencies.sync_parent == nullptr) {
		return false;
	}
	if (!has_valid_snapshot_frames(frames)) {
		return false;
	}
	const auto extension = snapshot_image_extension(path);
	if (extension.empty()) {
		return false;
	}

	if (!ensure_snapshot_directory(path.parent_path())) {
		return false;
	}

	auto staged = howdy::native::prepare_staged_file(
	    path, ".howdy-snapshot-", kSnapshotFileMode,
	    howdy::native::StagedFileMetadataPolicy::kUseDefaultMode);
	if (!staged.has_value()) {
		return false;
	}

	try {
		const int frame_height = frames.front().rows;
		cv::Mat   snap;
		cv::hconcat(frames, snap);
		cv::Mat padded;
		cv::copyMakeBorder(snap, padded, 0, (static_cast<int>(text_lines.size()) * 20) + 40, 0, 0,
		                   cv::BORDER_CONSTANT, cv::Scalar(44, 44, 44));
		snap = padded;

		for (std::size_t index = 0; index < text_lines.size(); ++index) {
			const int padding_top = frame_height + 30 + (static_cast<int>(index) * 20);
			cv::putText(snap, text_lines[index], cv::Point(30, padding_top),
			            cv::FONT_HERSHEY_SIMPLEX, 0.4, cv::Scalar(255, 255, 255), 0, cv::LINE_AA);
		}

		std::vector<uchar> encoded_bytes;
		if (!dependencies.encode_image(dependencies.context, extension, snap, &encoded_bytes) ||
		    encoded_bytes.empty() ||
		    !howdy::native::write_all_to_fd(staged->fd.get(),
		                                    reinterpret_cast<const char *>(encoded_bytes.data()),
		                                    encoded_bytes.size())) {
			howdy::native::cleanup_staged_file(*staged);
			return false;
		}
	} catch (...) {
		howdy::native::cleanup_staged_file(*staged);
		return false;
	}
	const auto result = howdy::native::install_staged_file(*staged, path, dependencies.sync_parent);
	if (commit_result != nullptr) {
		*commit_result = result;
	}
	return howdy::native::atomic_file_commit_is_durable(result);
}

auto howdy::native::snapshot_internal::snapshot_main_with_dependencies(
    int argc, char **argv, const SnapshotDependencies &dependencies) -> int {
	(void)argc;
	(void)argv;

	if (dependencies.load_runtime_config == nullptr || dependencies.capture_frames == nullptr ||
	    dependencies.write_snapshot == nullptr) {
		return kExitAbort;
	}

	auto config_result = dependencies.load_runtime_config(dependencies.context);
	if (config_result.status != howdy::native::RuntimeConfigLoadStatus::kOk ||
	    !config_result.config.has_value()) {
		std::cerr << config_result.error_message << "\n";
		return kExitAbort;
	}
	const auto &config = *config_result.config;

	auto capture_result = dependencies.capture_frames(dependencies.context, config);
	switch (capture_result.status) {
		case SnapshotCaptureStatus::kOk:
			break;
		case SnapshotCaptureStatus::kOpenError:
			std::cerr << capture_result.error_message << "\n";
			return kExitAbort;
		case SnapshotCaptureStatus::kReadError:
			std::cerr << "Could not capture a camera frame\n";
			return kExitAbort;
		default:
			std::cerr << "Internal error: unknown snapshot capture status\n";
			return kExitAbort;
	}

	if (capture_result.frames.size() != kSnapshotFrameCount) {
		std::cerr << "Internal error: snapshot capture returned unexpected frame count\n";
		return kExitAbort;
	}

	const auto write_result =
	    dependencies.write_snapshot(dependencies.context, capture_result.frames, config);
	if (!write_result.ok || write_result.path.empty()) {
		std::cerr << "Failed to write snapshot\n";
		return kExitAbort;
	}

	std::cout << "Snapshot saved to\n";
	std::cout << write_result.path.string() << "\n";
	return kExitOk;
}
