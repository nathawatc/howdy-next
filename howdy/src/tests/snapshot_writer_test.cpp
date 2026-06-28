#include "cli/snapshot_internal.hpp"
#include "common/atomic_files.hpp"
#include "common/frame_validation.hpp"

#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
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

	enum class CloseTempFdBehavior {
		kClose,
		kCloseThenFail,
		kCloseThenThrow,
	};

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
		bool                create_output          = false;
		bool                throw_cv_exception     = false;
		bool                throw_std_exception    = false;
		bool                throw_chmod_exception  = false;
		bool                throw_sync_exception   = false;
		bool                write_result           = true;
		bool                perform_real_chmod     = false;
		int                 chmod_result           = 0;
		CloseTempFdBehavior close_temp_fd_behavior = CloseTempFdBehavior::kClose;

		int write_calls         = 0;
		int chmod_calls         = 0;
		int sync_calls          = 0;
		int close_temp_fd_calls = 0;
		int temp_fd             = -1;
		int sentinel_fd         = -1;

		fs::path write_path;
		fs::path chmod_path;
		fs::path sync_path;
		mode_t   chmod_mode                   = 0;
		bool     sync_path_exists_during_sync = false;
		bool     temp_path_exists_during_sync = false;
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

	auto read_file(const fs::path &path) -> std::string {
		std::ifstream      input(path, std::ios::binary);
		std::ostringstream contents;
		contents << input.rdbuf();
		return contents.str();
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

	auto real_close_temp_fd(void *context, int fd) -> int {
		(void)context;
		return close(fd);
	}

	auto real_writer_dependencies() -> snapshot_internal::SnapshotWriterDependencies {
		return snapshot_internal::SnapshotWriterDependencies{
		    .context       = nullptr,
		    .write_image   = real_write_image,
		    .chmod_path    = real_chmod_path,
		    .sync_parent   = real_sync_parent,
		    .close_temp_fd = real_close_temp_fd,
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
		if (context->throw_cv_exception) {
			throw cv::Exception(cv::Error::StsError, "fake writer exception", "fake_write_image",
			                    __FILE__, __LINE__);
		}
		if (context->throw_std_exception) {
			throw std::runtime_error("fake writer exception");
		}
		return context->write_result;
	}

	auto fake_chmod_path(void *raw_context, const fs::path &path, mode_t mode) -> int {
		auto *context = static_cast<WriterCallbackContext *>(raw_context);
		++context->chmod_calls;
		context->chmod_path = path;
		context->chmod_mode = mode;
		if (context->throw_chmod_exception) {
			throw std::runtime_error("fake chmod exception");
		}
		if (context->perform_real_chmod) {
			return chmod(path.c_str(), mode);
		}
		return context->chmod_result;
	}

	auto fake_sync_parent(void *raw_context, const fs::path &path) -> void {
		auto *context = static_cast<WriterCallbackContext *>(raw_context);
		++context->sync_calls;
		context->sync_path                    = path;
		context->sync_path_exists_during_sync = fs::exists(path);
		context->temp_path_exists_during_sync = fs::exists(context->write_path);
		if (context->throw_sync_exception) {
			throw std::runtime_error("fake sync exception");
		}
	}

	auto fake_close_temp_fd(void *raw_context, int fd) -> int {
		auto *context = static_cast<WriterCallbackContext *>(raw_context);
		++context->close_temp_fd_calls;
		context->temp_fd                   = fd;
		const auto close_and_open_sentinel = [context, fd]() {
			if (close(fd) != 0) {
				return false;
			}
			context->sentinel_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
			return context->sentinel_fd >= 0;
		};
		switch (context->close_temp_fd_behavior) {
			case CloseTempFdBehavior::kClose:
				return close(fd);
			case CloseTempFdBehavior::kCloseThenFail:
				(void)close_and_open_sentinel();
				return -1;
			case CloseTempFdBehavior::kCloseThenThrow:
				(void)close_and_open_sentinel();
				throw std::runtime_error("fake close exception");
		}
		return -1;
	}

	auto fake_writer_dependencies(WriterCallbackContext &context)
	    -> snapshot_internal::SnapshotWriterDependencies {
		return snapshot_internal::SnapshotWriterDependencies{
		    .context       = &context,
		    .write_image   = fake_write_image,
		    .chmod_path    = fake_chmod_path,
		    .sync_parent   = fake_sync_parent,
		    .close_temp_fd = fake_close_temp_fd,
		};
	}

	auto consuming_close_failure_cleans_up(const std::string &name, CloseTempFdBehavior behavior)
	    -> bool {
		bool       ok        = true;
		const auto temp_root = make_temp_root(name, ok);
		const auto output    = temp_root / "log" / "snapshots" / "test.jpg";

		WriterCallbackContext context;
		context.close_temp_fd_behavior = behavior;

		const bool result = snapshot_internal::write_snapshot_at_path(
		    tiny_frames(), text_lines(), output, fake_writer_dependencies(context));

		const bool descriptor_reused =
		    context.temp_fd >= 0 && context.sentinel_fd == context.temp_fd;
		const bool sentinel_open =
		    context.sentinel_fd >= 0 && fcntl(context.sentinel_fd, F_GETFD) >= 0;
		const bool sentinel_closed = context.sentinel_fd >= 0 && close(context.sentinel_fd) == 0;
		errno                      = 0;
		const bool no_descriptor_leak =
		    context.sentinel_fd >= 0 && fcntl(context.sentinel_fd, F_GETFD) == -1 && errno == EBADF;
		std::error_code ec;
		const bool      directory_empty = fs::is_empty(output.parent_path(), ec);
		ok &= expect(!result, name + " returns false");
		ok &= expect(context.close_temp_fd_calls == 1, name + " calls injected closer once");
		ok &= expect(descriptor_reused, name + " reuses consumed descriptor for sentinel");
		ok &= expect(sentinel_open, name + " does not double-close reused descriptor");
		ok &= expect(sentinel_closed && no_descriptor_leak, name + " leaves no descriptor leak");
		ok &= expect(context.write_calls == 0, name + " skips writer");
		ok &= expect(!fs::exists(output), name + " creates no destination");
		ok &= expect(!ec && directory_empty, name + " removes raw temp output");

		return cleanup_temp_root(temp_root, ok);
	}

	auto close_temp_fd_error_after_close_cleans_up() -> bool {
		return consuming_close_failure_cleans_up("close-then-error",
		                                         CloseTempFdBehavior::kCloseThenFail);
	}

	auto close_temp_fd_exception_after_close_cleans_up() -> bool {
		return consuming_close_failure_cleans_up("close-then-exception",
		                                         CloseTempFdBehavior::kCloseThenThrow);
	}

	auto expect_invalid_batch_rejects_before_lifecycle(const std::string          &name,
	                                                   const std::vector<cv::Mat> &frames) -> bool {
		bool       ok        = true;
		const auto temp_root = make_temp_root(name, ok);
		const auto log_root  = temp_root / "log";
		const auto output    = log_root / "snapshots" / "test.jpg";

		WriterCallbackContext context;
		context.create_output = true;

		const bool result = snapshot_internal::write_snapshot_at_path(
		    frames, text_lines(), output, fake_writer_dependencies(context));

		ok &= expect(!result, name + " returns false");
		ok &= expect(context.write_calls == 0, name + " skips writer");
		ok &= expect(context.chmod_calls == 0, name + " skips chmod");
		ok &= expect(context.sync_calls == 0, name + " skips sync");
		ok &= expect(!fs::exists(log_root), name + " creates no log directory");
		ok &= expect(!fs::exists(output), name + " creates no output");

		return cleanup_temp_root(temp_root, ok);
	}

	auto empty_member_rejects_before_lifecycle() -> bool {
		auto frames = tiny_frames();
		frames[1]   = cv::Mat{};
		return expect_invalid_batch_rejects_before_lifecycle("empty-member", frames);
	}

	auto wrong_channel_rejects_before_lifecycle() -> bool {
		return expect_invalid_batch_rejects_before_lifecycle(
		    "wrong-channel", {cv::Mat(8, 8, CV_8UC1, cv::Scalar(10))});
	}

	auto wrong_pixel_depth_rejects_before_lifecycle() -> bool {
		return expect_invalid_batch_rejects_before_lifecycle(
		    "wrong-pixel-depth", {cv::Mat(8, 8, CV_16UC3, cv::Scalar(10, 20, 30))});
	}

	auto combined_width_rejects_before_lifecycle() -> bool {
		const auto frames = std::vector<cv::Mat>{
		    cv::Mat(1, 4097, CV_8UC3, cv::Scalar(10, 20, 30)),
		    cv::Mat(1, 4097, CV_8UC3, cv::Scalar(40, 50, 60)),
		};
		bool ok = true;
		for (const auto &frame : frames) {
			ok &= expect(
			    howdy::native::validate_frame(frame, howdy::native::FrameChannelPolicy::kBgr) ==
			        howdy::native::FrameValidationStatus::kValid,
			    "combined-width frame individually passes shared validation");
		}
		return ok && expect_invalid_batch_rejects_before_lifecycle("combined-width", frames);
	}

	auto oversized_frame_rejects_before_lifecycle() -> bool {
		return expect_invalid_batch_rejects_before_lifecycle(
		    "oversized-frame",
		    {cv::Mat(howdy::native::kMaxFrameDimension + 1, 1, CV_8UC3, cv::Scalar(10, 20, 30))});
	}

	auto shape_mismatch_rejects_before_lifecycle() -> bool {
		return expect_invalid_batch_rejects_before_lifecycle(
		    "shape-mismatch", {cv::Mat(8, 8, CV_8UC3, cv::Scalar(10, 20, 30)),
		                       cv::Mat(9, 8, CV_8UC3, cv::Scalar(40, 50, 60))});
	}

	auto mixed_width_frames_are_supported() -> bool {
		bool       ok        = true;
		const auto temp_root = make_temp_root("mixed-width", ok);
		const auto output    = temp_root / "log" / "snapshots" / "test.jpg";

		WriterCallbackContext context;
		context.create_output      = true;
		context.perform_real_chmod = true;

		const bool result = snapshot_internal::write_snapshot_at_path(
		    {cv::Mat(8, 4, CV_8UC3, cv::Scalar(10, 20, 30)),
		     cv::Mat(8, 12, CV_8UC3, cv::Scalar(40, 50, 60))},
		    text_lines(), output, fake_writer_dependencies(context));

		ok &= expect(result, "mixed-width frames return true");
		ok &= expect(context.write_calls == 1, "mixed-width calls writer once");
		ok &= expect(context.write_path.parent_path() == output.parent_path(),
		             "mixed-width writes temp in target directory");
		ok &= expect(!fs::exists(context.write_path), "mixed-width temp is renamed away");
		ok &= expect(fs::exists(output), "mixed-width creates output");

		return cleanup_temp_root(temp_root, ok);
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

	auto png_destination_uses_png_encoding() -> bool {
		bool       ok        = true;
		const auto temp_root = make_temp_root("png-encoding", ok);
		const auto output    = temp_root / "log" / "snapshots" / "test.png";

		const bool result = snapshot_internal::write_snapshot_at_path(
		    tiny_frames(), text_lines(), output, real_writer_dependencies());
		const auto contents = read_file(output);

		ok &= expect(result, "PNG destination returns true");
		ok &= expect(fs::exists(output), "PNG destination exists after rename");
		ok &= expect(contents.size() >= 8 && contents.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0,
		             "PNG destination contains PNG encoding");

		return cleanup_temp_root(temp_root, ok);
	}

	auto hidden_png_destination_uses_png_encoding() -> bool {
		bool       ok        = true;
		const auto temp_root = make_temp_root("hidden-png-encoding", ok);
		const auto output    = temp_root / "log" / "snapshots" / ".png";

		const bool result = snapshot_internal::write_snapshot_at_path(
		    tiny_frames(), text_lines(), output, real_writer_dependencies());
		const auto contents = read_file(output);

		ok &= expect(result, "hidden PNG destination returns true");
		ok &= expect(fs::exists(output), "hidden PNG destination exists after rename");
		ok &= expect(contents.size() >= 8 && contents.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0,
		             "hidden PNG destination contains PNG encoding");

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
		ok &= expect(!fs::exists(output), "image write failure creates no target output");
		ok &= expect(!fs::exists(context.write_path), "image write failure removes temp output");

		return cleanup_temp_root(temp_root, ok);
	}

	auto writer_failure_preserves_preexisting_output() -> bool {
		bool       ok        = true;
		const auto temp_root = make_temp_root("writer-failure-preexisting", ok);
		const auto output    = temp_root / "log" / "snapshots" / "test.jpg";
		const auto original  = std::string("original snapshot contents");

		std::error_code ec;
		fs::create_directories(output.parent_path(), ec);
		ok &= expect(!ec, "writer failure preexisting creates parent directory");
		ok &= expect(write_file(output, original),
		             "writer failure preexisting creates original output");

		WriterCallbackContext context;
		context.create_output = true;
		context.write_result  = false;

		const bool result = snapshot_internal::write_snapshot_at_path(
		    tiny_frames(), text_lines(), output, fake_writer_dependencies(context));

		ok &= expect(!result, "writer failure preexisting returns false");
		ok &= expect(context.write_calls == 1, "writer failure preexisting calls writer once");
		ok &= expect(context.chmod_calls == 0, "writer failure preexisting skips chmod");
		ok &= expect(context.sync_calls == 0, "writer failure preexisting skips sync");
		ok &= expect(!fs::exists(context.write_path),
		             "writer failure preexisting removes temp output");
		ok &= expect(fs::exists(output), "writer failure preexisting keeps original output");
		ok &= expect(read_file(output) == original,
		             "writer failure preexisting leaves original output unchanged");

		return cleanup_temp_root(temp_root, ok);
	}

	auto writer_std_exception_preserves_preexisting_output() -> bool {
		bool       ok        = true;
		const auto temp_root = make_temp_root("writer-std-exception-preexisting", ok);
		const auto output    = temp_root / "log" / "snapshots" / "test.jpg";
		const auto original  = std::string("original snapshot contents");

		std::error_code ec;
		fs::create_directories(output.parent_path(), ec);
		ok &= expect(!ec, "writer std exception preexisting creates parent directory");
		ok &= expect(write_file(output, original),
		             "writer std exception preexisting creates original output");

		WriterCallbackContext context;
		context.create_output       = true;
		context.throw_std_exception = true;

		const bool result = snapshot_internal::write_snapshot_at_path(
		    tiny_frames(), text_lines(), output, fake_writer_dependencies(context));

		ok &= expect(!result, "writer std exception preexisting returns false");
		ok &=
		    expect(context.write_calls == 1, "writer std exception preexisting calls writer once");
		ok &= expect(context.chmod_calls == 0, "writer std exception preexisting skips chmod");
		ok &= expect(context.sync_calls == 0, "writer std exception preexisting skips sync");
		ok &= expect(!fs::exists(context.write_path),
		             "writer std exception preexisting removes temp output");
		ok &= expect(fs::exists(output), "writer std exception preexisting keeps original output");
		ok &= expect(read_file(output) == original,
		             "writer std exception preexisting leaves original output unchanged");

		return cleanup_temp_root(temp_root, ok);
	}

	auto writer_cv_exception_preserves_preexisting_output() -> bool {
		bool       ok        = true;
		const auto temp_root = make_temp_root("writer-cv-exception-preexisting", ok);
		const auto output    = temp_root / "log" / "snapshots" / "test.jpg";
		const auto original  = std::string("original snapshot contents");

		std::error_code ec;
		fs::create_directories(output.parent_path(), ec);
		ok &= expect(!ec, "writer cv exception preexisting creates parent directory");
		ok &= expect(write_file(output, original),
		             "writer cv exception preexisting creates original output");

		WriterCallbackContext context;
		context.throw_cv_exception = true;

		const bool result = snapshot_internal::write_snapshot_at_path(
		    tiny_frames(), text_lines(), output, fake_writer_dependencies(context));

		ok &= expect(!result, "writer cv exception preexisting returns false");
		ok &= expect(context.write_calls == 1, "writer cv exception preexisting calls writer once");
		ok &= expect(context.chmod_calls == 0, "writer cv exception preexisting skips chmod");
		ok &= expect(context.sync_calls == 0, "writer cv exception preexisting skips sync");
		ok &= expect(!fs::exists(context.write_path),
		             "writer cv exception preexisting removes temp output");
		ok &= expect(fs::exists(output), "writer cv exception preexisting keeps original output");
		ok &= expect(read_file(output) == original,
		             "writer cv exception preexisting leaves original output unchanged");

		return cleanup_temp_root(temp_root, ok);
	}

	auto writer_cv_exception_removes_created_output() -> bool {
		bool       ok        = true;
		const auto temp_root = make_temp_root("writer-cv-exception", ok);
		const auto output    = temp_root / "log" / "snapshots" / "test.jpg";

		WriterCallbackContext context;
		context.create_output      = true;
		context.throw_cv_exception = true;

		const bool result = snapshot_internal::write_snapshot_at_path(
		    tiny_frames(), text_lines(), output, fake_writer_dependencies(context));

		ok &= expect(!result, "writer cv exception returns false");
		ok &= expect(context.write_calls == 1, "writer cv exception calls writer once");
		ok &= expect(context.chmod_calls == 0, "writer cv exception skips chmod");
		ok &= expect(context.sync_calls == 0, "writer cv exception skips sync");
		ok &= expect(!fs::exists(output), "writer cv exception creates no target output");
		ok &= expect(!fs::exists(context.write_path), "writer cv exception removes temp output");

		return cleanup_temp_root(temp_root, ok);
	}

	auto chmod_failure_preserves_preexisting_output() -> bool {
		bool       ok        = true;
		const auto temp_root = make_temp_root("chmod-failure-preexisting", ok);
		const auto output    = temp_root / "log" / "snapshots" / "test.jpg";
		const auto original  = std::string("original snapshot contents");

		std::error_code ec;
		fs::create_directories(output.parent_path(), ec);
		ok &= expect(!ec, "chmod failure preexisting creates parent directory");
		ok &= expect(write_file(output, original),
		             "chmod failure preexisting creates original output");

		WriterCallbackContext context;
		context.create_output = true;
		context.chmod_result  = -1;

		const bool result = snapshot_internal::write_snapshot_at_path(
		    tiny_frames(), text_lines(), output, fake_writer_dependencies(context));

		ok &= expect(!result, "chmod failure preexisting returns false");
		ok &= expect(context.write_calls == 1, "chmod failure preexisting calls writer once");
		ok &= expect(context.chmod_calls == 1, "chmod failure preexisting calls chmod once");
		ok &= expect(context.sync_calls == 0, "chmod failure preexisting skips sync");
		ok &= expect(!fs::exists(context.write_path),
		             "chmod failure preexisting removes temp output");
		ok &= expect(fs::exists(output), "chmod failure preexisting keeps original output");
		ok &= expect(read_file(output) == original,
		             "chmod failure preexisting leaves original output unchanged");

		return cleanup_temp_root(temp_root, ok);
	}

	auto chmod_exception_preserves_preexisting_output() -> bool {
		bool       ok        = true;
		const auto temp_root = make_temp_root("chmod-exception-preexisting", ok);
		const auto output    = temp_root / "log" / "snapshots" / "test.jpg";
		const auto original  = std::string("original snapshot contents");

		std::error_code ec;
		fs::create_directories(output.parent_path(), ec);
		ok &= expect(!ec, "chmod exception preexisting creates parent directory");
		ok &= expect(write_file(output, original),
		             "chmod exception preexisting creates original output");

		WriterCallbackContext context;
		context.create_output         = true;
		context.throw_chmod_exception = true;

		const bool result = snapshot_internal::write_snapshot_at_path(
		    tiny_frames(), text_lines(), output, fake_writer_dependencies(context));

		ok &= expect(!result, "chmod exception preexisting returns false");
		ok &= expect(context.write_calls == 1, "chmod exception preexisting calls writer once");
		ok &= expect(context.chmod_calls == 1, "chmod exception preexisting calls chmod once");
		ok &= expect(context.sync_calls == 0, "chmod exception preexisting skips sync");
		ok &= expect(!fs::exists(context.write_path),
		             "chmod exception preexisting removes temp output");
		ok &= expect(fs::exists(output), "chmod exception preexisting keeps original output");
		ok &= expect(read_file(output) == original,
		             "chmod exception preexisting leaves original output unchanged");

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
		ok &= expect(!fs::exists(output), "chmod failure creates no target output");
		ok &= expect(!fs::exists(context.write_path), "chmod failure removes temp output");

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
		ok &= expect(context.write_path.parent_path() == output.parent_path(),
		             "successful callback lifecycle writes temp in target directory");
		ok &= expect(context.chmod_path == context.write_path,
		             "successful callback lifecycle chmods temp path");
		ok &= expect(context.sync_path == output,
		             "successful callback lifecycle syncs destination path");
		ok &= expect(context.sync_path_exists_during_sync,
		             "successful callback lifecycle syncs after destination exists");
		ok &= expect(!context.temp_path_exists_during_sync,
		             "successful callback lifecycle syncs after temp rename");
		ok &= expect(!fs::exists(context.write_path),
		             "successful callback lifecycle renames temp away");
		ok &= expect(fs::exists(output), "successful callback lifecycle keeps output");

		return cleanup_temp_root(temp_root, ok);
	}

	auto sync_exception_keeps_renamed_output() -> bool {
		bool       ok        = true;
		const auto temp_root = make_temp_root("sync-exception", ok);
		const auto output    = temp_root / "log" / "snapshots" / "test.jpg";

		WriterCallbackContext context;
		context.create_output        = true;
		context.perform_real_chmod   = true;
		context.throw_sync_exception = true;

		const bool result = snapshot_internal::write_snapshot_at_path(
		    tiny_frames(), text_lines(), output, fake_writer_dependencies(context));

		ok &= expect(!result, "sync exception returns false");
		ok &= expect(context.sync_calls == 1, "sync exception calls sync once");
		ok &= expect(context.sync_path_exists_during_sync,
		             "sync exception runs after destination rename");
		ok &=
		    expect(!context.temp_path_exists_during_sync, "sync exception runs after temp rename");
		ok &= expect(!fs::exists(context.write_path), "sync exception leaves no temp output");
		ok &= expect(fs::exists(output), "sync exception keeps renamed destination");
		ok &= expect(read_file(output) == "fake snapshot",
		             "sync exception keeps renamed destination contents");

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
	ok &= empty_member_rejects_before_lifecycle();
	ok &= wrong_channel_rejects_before_lifecycle();
	ok &= wrong_pixel_depth_rejects_before_lifecycle();
	ok &= combined_width_rejects_before_lifecycle();
	ok &= oversized_frame_rejects_before_lifecycle();
	ok &= shape_mismatch_rejects_before_lifecycle();
	ok &= close_temp_fd_error_after_close_cleans_up();
	ok &= close_temp_fd_exception_after_close_cleans_up();
	ok &= mixed_width_frames_are_supported();
	ok &= successful_real_writer_secures_output();
	ok &= png_destination_uses_png_encoding();
	ok &= hidden_png_destination_uses_png_encoding();
	ok &= insecure_existing_log_root_rejects_before_write();
	ok &= image_write_failure_stops_lifecycle();
	ok &= writer_failure_preserves_preexisting_output();
	ok &= writer_std_exception_preserves_preexisting_output();
	ok &= writer_cv_exception_preserves_preexisting_output();
	ok &= writer_cv_exception_removes_created_output();
	ok &= chmod_failure_preserves_preexisting_output();
	ok &= chmod_exception_preserves_preexisting_output();
	ok &= chmod_failure_removes_created_output();
	ok &= successful_callback_lifecycle_records_calls();
	ok &= sync_exception_keeps_renamed_output();
	ok &= missing_parent_directories_are_created_and_secured();
	return ok ? 0 : 1;
}
