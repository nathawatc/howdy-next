#include "cli/snapshot_internal.hpp"
#include "common/atomic_files.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <sys/stat.h>

namespace {

	constexpr mode_t kSnapshotDirectoryMode = S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP;
	constexpr mode_t kSnapshotFileMode      = S_IRUSR | S_IWUSR;

	namespace fs                = std::filesystem;
	namespace snapshot_internal = howdy::native::snapshot_internal;

	struct StreamRedirect {
		StreamRedirect(std::ostream &stream, std::streambuf *new_buffer)
		    : output(stream)
		    , old_buffer(stream.rdbuf(new_buffer)) {}

		~StreamRedirect() {
			output.rdbuf(old_buffer);
		}

		std::ostream   &output;
		std::streambuf *old_buffer;
	};

	struct WriterCallbackContext {
		bool create_output      = false;
		bool write_result       = true;
		bool perform_real_chmod = false;
		int  chmod_result       = 0;

		int write_calls = 0;
		int chmod_calls = 0;
		int sync_calls  = 0;

		fs::path write_path;
		fs::path chmod_path;
		fs::path sync_path;
		mode_t   chmod_mode = 0;
	};

	auto expect(bool condition, const std::string &message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << "\n";
			return false;
		}
		return true;
	}

	auto make_temp_root(const std::string &name, bool &ok) -> fs::path {
		const auto root =
		    fs::current_path() / ("howdy-snapshot-writer-" + name + "-" + std::to_string(getpid()));
		std::error_code ec;
		fs::remove_all(root, ec);
		ok &= expect(!ec, "remove stale temp root for " + name);
		fs::create_directories(root, ec);
		ok &= expect(!ec, "create temp root for " + name);
		ok &= expect(chmod(root.c_str(), kSnapshotDirectoryMode) == 0,
		             "secure temp root for " + name);
		return root;
	}

	auto cleanup_temp_root(const fs::path &root, bool ok) -> bool {
		if (!ok) {
			std::cerr << "Leaving snapshot writer test artifacts: " << root << "\n";
			return false;
		}

		std::error_code ec;
		fs::remove_all(root, ec);
		return expect(!ec, "remove temp root " + root.string());
	}

	auto path_mode(const fs::path &path) -> std::optional<mode_t> {
		struct stat stat_{};
		if (stat(path.c_str(), &stat_) != 0) {
			return std::nullopt;
		}
		return stat_.st_mode & 0777;
	}

	auto expect_mode(const fs::path &path, mode_t expected_mode, const std::string &message)
	    -> bool {
		const auto mode = path_mode(path);
		if (!mode.has_value()) {
			return expect(false, "stat " + message);
		}
		return expect(*mode == expected_mode, message);
	}

	auto write_file(const fs::path &path, const std::string &contents) -> bool {
		std::ofstream output(path, std::ios::binary);
		if (!output) {
			return false;
		}
		output << contents;
		return static_cast<bool>(output);
	}

	auto tiny_frames() -> std::vector<cv::Mat> {
		return {
		    cv::Mat(8, 8, CV_8UC3, cv::Scalar(10, 20, 30)),
		    cv::Mat(8, 8, CV_8UC3, cv::Scalar(40, 50, 60)),
		    cv::Mat(8, 8, CV_8UC3, cv::Scalar(70, 80, 90)),
		    cv::Mat(8, 8, CV_8UC3, cv::Scalar(100, 110, 120)),
		};
	}

	auto text_lines() -> std::vector<std::string> {
		return {"snapshot writer test"};
	}

	auto real_write_image(void *context, const fs::path &path, const cv::Mat &image) -> bool {
		(void)context;
		return cv::imwrite(path.string(), image);
	}

	auto real_chmod_path(void *context, const fs::path &path, mode_t mode) -> int {
		(void)context;
		return chmod(path.c_str(), mode);
	}

	auto real_sync_parent(void *context, const fs::path &path) -> void {
		(void)context;
		howdy::native::sync_parent_directory(path);
	}

	auto real_writer_dependencies() -> snapshot_internal::SnapshotWriterDependencies {
		return snapshot_internal::SnapshotWriterDependencies{
		    .context     = nullptr,
		    .write_image = real_write_image,
		    .chmod_path  = real_chmod_path,
		    .sync_parent = real_sync_parent,
		};
	}

	auto fake_write_image(void *raw_context, const fs::path &path, const cv::Mat &image) -> bool {
		(void)image;
		auto *context = static_cast<WriterCallbackContext *>(raw_context);
		++context->write_calls;
		context->write_path = path;
		if (context->create_output && !write_file(path, "fake snapshot")) {
			return false;
		}
		return context->write_result;
	}

	auto fake_chmod_path(void *raw_context, const fs::path &path, mode_t mode) -> int {
		auto *context = static_cast<WriterCallbackContext *>(raw_context);
		++context->chmod_calls;
		context->chmod_path = path;
		context->chmod_mode = mode;
		if (context->perform_real_chmod) {
			return chmod(path.c_str(), mode);
		}
		return context->chmod_result;
	}

	auto fake_sync_parent(void *raw_context, const fs::path &path) -> void {
		auto *context = static_cast<WriterCallbackContext *>(raw_context);
		++context->sync_calls;
		context->sync_path = path;
	}

	auto fake_writer_dependencies(WriterCallbackContext &context)
	    -> snapshot_internal::SnapshotWriterDependencies {
		return snapshot_internal::SnapshotWriterDependencies{
		    .context     = &context,
		    .write_image = fake_write_image,
		    .chmod_path  = fake_chmod_path,
		    .sync_parent = fake_sync_parent,
		};
	}

	auto successful_real_writer_secures_output() -> bool {
		bool       ok        = true;
		const auto temp_root = make_temp_root("successful-real-writer", ok);
		const auto log_root  = temp_root / "log";
		const auto output    = log_root / "snapshots" / "test.jpg";

		std::error_code ec;
		fs::create_directories(log_root, ec);
		ok &= expect(!ec, "create log root for real writer");
		ok &= expect(chmod(log_root.c_str(), kSnapshotDirectoryMode) == 0,
		             "secure log root for real writer");

		const bool result = snapshot_internal::write_snapshot_at_path(
		    tiny_frames(), text_lines(), output, real_writer_dependencies());

		ok &= expect(result, "successful real writer returns true");
		ok &= expect(fs::exists(output), "successful real writer creates output");
		ok &= expect_mode(output, kSnapshotFileMode, "output mode is 0600");
		ok &= expect_mode(log_root, kSnapshotDirectoryMode, "log root mode is 0750");
		ok &= expect_mode(output.parent_path(), kSnapshotDirectoryMode,
		                  "snapshots directory mode is 0750");
		const auto file_size = fs::file_size(output, ec);
		ok &= expect(!ec && file_size > 0, "successful real writer creates non-empty output");

		return cleanup_temp_root(temp_root, ok);
	}

	auto insecure_existing_log_root_rejects_before_write() -> bool {
		bool       ok        = true;
		const auto temp_root = make_temp_root("insecure-log-root", ok);
		const auto log_root  = temp_root / "log";
		const auto output    = log_root / "snapshots" / "test.jpg";

		std::error_code ec;
		fs::create_directories(log_root, ec);
		ok &= expect(!ec, "create insecure log root");
		ok &=
		    expect(chmod(log_root.c_str(), S_IRWXU | S_IRWXG) == 0, "make log root group-writable");

		WriterCallbackContext context;
		context.create_output = true;
		std::ostringstream error;
		{
			StreamRedirect redirect(std::cerr, error.rdbuf());
			const bool     result = snapshot_internal::write_snapshot_at_path(
			    tiny_frames(), text_lines(), output, fake_writer_dependencies(context));
			ok &= expect(!result, "insecure log root returns false");
		}

		ok &= expect(context.write_calls == 0, "insecure log root skips writer");
		ok &= expect(!fs::exists(output), "insecure log root creates no output");
		ok &= expect(error.str().contains("Log directory must not be group-writable"),
		             "insecure log root reports secure-directory diagnostic");

		return cleanup_temp_root(temp_root, ok);
	}

	auto image_write_failure_stops_lifecycle() -> bool {
		bool       ok        = true;
		const auto temp_root = make_temp_root("image-write-failure", ok);
		const auto output    = temp_root / "log" / "snapshots" / "test.jpg";

		WriterCallbackContext context;
		context.create_output = true;
		context.write_result  = false;

		const bool result = snapshot_internal::write_snapshot_at_path(
		    tiny_frames(), text_lines(), output, fake_writer_dependencies(context));

		ok &= expect(!result, "image write failure returns false");
		ok &= expect(context.write_calls == 1, "image write failure calls writer once");
		ok &= expect(context.chmod_calls == 0, "image write failure skips chmod");
		ok &= expect(context.sync_calls == 0, "image write failure skips sync");
		ok &= expect(!fs::exists(output), "image write failure removes partial output");

		return cleanup_temp_root(temp_root, ok);
	}

	auto chmod_failure_removes_created_output() -> bool {
		bool       ok        = true;
		const auto temp_root = make_temp_root("chmod-failure", ok);
		const auto output    = temp_root / "log" / "snapshots" / "test.jpg";

		WriterCallbackContext context;
		context.create_output = true;
		context.chmod_result  = -1;

		const bool result = snapshot_internal::write_snapshot_at_path(
		    tiny_frames(), text_lines(), output, fake_writer_dependencies(context));

		ok &= expect(!result, "chmod failure returns false");
		ok &= expect(context.write_calls == 1, "chmod failure calls writer once");
		ok &= expect(context.chmod_calls == 1, "chmod failure calls chmod once");
		ok &= expect(context.sync_calls == 0, "chmod failure skips sync");
		ok &= expect(!fs::exists(output), "chmod failure removes output");

		return cleanup_temp_root(temp_root, ok);
	}

	auto successful_callback_lifecycle_records_calls() -> bool {
		bool       ok        = true;
		const auto temp_root = make_temp_root("callback-lifecycle", ok);
		const auto output    = temp_root / "log" / "snapshots" / "test.jpg";

		WriterCallbackContext context;
		context.create_output      = true;
		context.perform_real_chmod = true;

		const bool result = snapshot_internal::write_snapshot_at_path(
		    tiny_frames(), text_lines(), output, fake_writer_dependencies(context));

		ok &= expect(result, "successful callback lifecycle returns true");
		ok &= expect(context.write_calls == 1, "successful callback lifecycle calls writer once");
		ok &= expect(context.chmod_calls == 1, "successful callback lifecycle calls chmod once");
		ok &= expect(context.chmod_mode == kSnapshotFileMode,
		             "successful callback lifecycle chmods 0600");
		ok &= expect(context.sync_calls == 1, "successful callback lifecycle calls sync once");
		ok &=
		    expect(context.sync_path == output, "successful callback lifecycle syncs target path");
		ok &= expect(fs::exists(output), "successful callback lifecycle keeps output");

		return cleanup_temp_root(temp_root, ok);
	}

	auto missing_parent_directories_are_created_and_secured() -> bool {
		bool       ok            = true;
		const auto temp_root     = make_temp_root("directory-creation", ok);
		const auto log_root      = temp_root / "log";
		const auto snapshots_dir = log_root / "snapshots";
		const auto output        = snapshots_dir / "test.jpg";

		ok &= expect(!fs::exists(snapshots_dir), "directory creation starts without snapshots dir");

		WriterCallbackContext context;
		context.create_output      = true;
		context.perform_real_chmod = true;

		const bool result = snapshot_internal::write_snapshot_at_path(
		    tiny_frames(), text_lines(), output, fake_writer_dependencies(context));

		ok &= expect(result, "directory creation writer returns true");
		ok &= expect(context.write_calls == 1, "directory creation calls writer once");
		ok &= expect(fs::exists(log_root), "directory creation creates log root");
		ok &= expect(fs::exists(snapshots_dir), "directory creation creates snapshots dir");
		ok &= expect_mode(log_root, kSnapshotDirectoryMode, "created log root mode is 0750");
		ok &= expect_mode(snapshots_dir, kSnapshotDirectoryMode,
		                  "created snapshots directory mode is 0750");
		ok &= expect(fs::exists(output), "directory creation creates output");

		return cleanup_temp_root(temp_root, ok);
	}

}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= successful_real_writer_secures_output();
	ok &= insecure_existing_log_root_rejects_before_write();
	ok &= image_write_failure_stops_lifecycle();
	ok &= chmod_failure_removes_created_output();
	ok &= successful_callback_lifecycle_records_calls();
	ok &= missing_parent_directories_are_created_and_secured();
	return ok ? 0 : 1;
}
