#include "cli/snapshot_cli.hpp"
#include "cli/snapshot_internal.hpp"
#include "common/atomic_files.hpp"
#include "common/file_security.hpp"
#include "common/frame_validation.hpp"
#include "config/runtime_config.hpp"
#include "config/runtime_paths.hpp"
#include "recorders/video_capture.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <sys/stat.h>

namespace {

	constexpr int         kExitOk                = 0;
	constexpr int         kExitAbort             = 1;
	constexpr std::size_t kSnapshotFrameCount    = 4;
	constexpr mode_t      kSnapshotDirectoryMode = S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP;
	constexpr mode_t      kSnapshotFileMode      = S_IRUSR | S_IWUSR;

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

	auto write_image_dependency(void *context, const std::filesystem::path &path,
	                            const cv::Mat &image) -> bool {
		(void)context;
		return cv::imwrite(path.string(), image);
	}

	auto chmod_path_dependency(void *context, const std::filesystem::path &path, mode_t mode)
	    -> int {
		(void)context;
		return chmod(path.c_str(), mode);
	}

	auto sync_parent_dependency(void *context, const std::filesystem::path &path) -> void {
		(void)context;
		howdy::native::sync_parent_directory(path);
	}

	auto close_temp_fd_dependency(void *context, int fd) -> int {
		(void)context;
		return close(fd);
	}

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

	class RawSnapshotTempPathGuard {
	public:
		explicit RawSnapshotTempPathGuard(const char *path) noexcept
		    : path_(path) {}

		~RawSnapshotTempPathGuard() {
			if (active_) {
				unlink(path_);
			}
		}

		RawSnapshotTempPathGuard(const RawSnapshotTempPathGuard &)                     = delete;
		auto operator=(const RawSnapshotTempPathGuard &) -> RawSnapshotTempPathGuard & = delete;

		void release() noexcept {
			active_ = false;
		}

	private:
		const char *path_;
		bool        active_ = true;
	};

	auto make_snapshot_temp_path(const std::filesystem::path &path, void *context,
	                             snapshot_internal::SnapshotCloseFdFn close_temp_fd)
	    -> std::filesystem::path {
		try {
			const auto filename     = path.filename().string();
			const auto suffix_start = filename.rfind('.');
			const auto suffix =
			    suffix_start == std::string::npos ? std::string{} : filename.substr(suffix_start);
			std::string temp_template =
			    (path.parent_path() / ("." + filename + ".tmp-XXXXXX" + suffix)).string();
			std::vector<char> writable(temp_template.begin(), temp_template.end());
			writable.push_back('\0');

			const int fd = mkstemps(writable.data(), static_cast<int>(suffix.size()));
			if (fd < 0) {
				return {};
			}
			RawSnapshotTempPathGuard cleanup(writable.data());
			if (close_temp_fd(context, fd) != 0) {
				return {};
			}

			std::filesystem::path temp_path(writable.data());
			cleanup.release();
			return temp_path;
		} catch (...) {
			return {};
		}
	}

	auto fsync_file_at_path(const std::filesystem::path &path) -> bool {
		const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
		if (fd < 0) {
			return false;
		}
		const bool ok = fsync(fd) == 0;
		close(fd);
		return ok;
	}

	class SnapshotTempPathGuard {
	public:
		explicit SnapshotTempPathGuard(const std::filesystem::path &path) noexcept
		    : path_(path) {}

		~SnapshotTempPathGuard() {
			if (!active_) {
				return;
			}
			std::error_code ec;
			std::filesystem::remove(path_, ec);
		}

		SnapshotTempPathGuard(const SnapshotTempPathGuard &)                     = delete;
		auto operator=(const SnapshotTempPathGuard &) -> SnapshotTempPathGuard & = delete;

		void release() noexcept {
			active_ = false;
		}

	private:
		const std::filesystem::path &path_;
		bool                         active_ = true;
	};

	auto generate_snapshot(const std::vector<cv::Mat>     &frames,
	                       const std::vector<std::string> &text_lines) -> std::filesystem::path {
		auto       filepath     = snapshot_path();
		const auto dependencies = snapshot_internal::SnapshotWriterDependencies{
		    .context       = nullptr,
		    .write_image   = write_image_dependency,
		    .chmod_path    = chmod_path_dependency,
		    .sync_parent   = sync_parent_dependency,
		    .close_temp_fd = close_temp_fd_dependency,
		};
		if (!snapshot_internal::write_snapshot_at_path(frames, text_lines, filepath,
		                                               dependencies)) {
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
		frames.reserve(kSnapshotFrameCount);
		while (frames.size() < kSnapshotFrameCount) {
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
    const std::filesystem::path &path, const SnapshotWriterDependencies &dependencies) -> bool {
	if (dependencies.write_image == nullptr || dependencies.chmod_path == nullptr ||
	    dependencies.sync_parent == nullptr || dependencies.close_temp_fd == nullptr) {
		return false;
	}
	if (!has_valid_snapshot_frames(frames)) {
		return false;
	}

	if (!ensure_snapshot_directory(path.parent_path())) {
		return false;
	}

	const auto temp_path =
	    make_snapshot_temp_path(path, dependencies.context, dependencies.close_temp_fd);
	if (temp_path.empty()) {
		return false;
	}
	SnapshotTempPathGuard temp_path_guard(temp_path);

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

		if (!dependencies.write_image(dependencies.context, temp_path, snap)) {
			return false;
		}
		if (dependencies.chmod_path(dependencies.context, temp_path, kSnapshotFileMode) != 0) {
			return false;
		}
		if (!fsync_file_at_path(temp_path)) {
			return false;
		}

		std::filesystem::rename(temp_path, path);
		temp_path_guard.release();
	} catch (...) {
		return false;
	}
	try {
		dependencies.sync_parent(dependencies.context, path);
	} catch (...) {
		return false;
	}
	return true;
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
			std::cerr << "Failed to read frame from camera\n";
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

	std::cout << "Generated snapshot saved as\n";
	std::cout << write_result.path.string() << "\n";
	return kExitOk;
}

auto snapshot_main(int argc, char **argv) -> int {
	return howdy::native::snapshot_internal::snapshot_main_with_dependencies(
	    argc, argv,
	    howdy::native::snapshot_internal::SnapshotDependencies{
	        .load_runtime_config = load_runtime_config_dependency,
	        .capture_frames      = capture_frames_dependency,
	        .write_snapshot      = write_snapshot_dependency,
	    });
}
