#include "cli/snapshot_internal.hpp"
#include "common/frame_validation.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
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
		bool create_encoded_output = true;
		bool throw_cv_exception    = false;
		bool throw_std_exception   = false;
		bool encode_result         = true;

		int encode_calls = 0;

		std::string        received_extension;
		std::vector<uchar> encoded_output;
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
		return expect(mode.has_value() && *mode == expected_mode, message);
	}

	auto write_file(const fs::path &path, std::string_view contents) -> bool {
		std::ofstream output(path, std::ios::binary);
		output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
		return static_cast<bool>(output);
	}

	auto read_file(const fs::path &path) -> std::string {
		std::ifstream      input(path, std::ios::binary);
		std::ostringstream contents;
		contents << input.rdbuf();
		return contents.str();
	}

	auto count_staged_files(const fs::path &directory) -> std::size_t {
		std::error_code ec;
		if (!fs::is_directory(directory, ec) || ec) {
			return 0;
		}
		std::size_t count = 0;
		for (const auto &entry : fs::directory_iterator(directory)) {
			if (entry.path().filename().string().starts_with(".howdy-snapshot-")) {
				++count;
			}
		}
		return count;
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

	auto fake_encode(void *raw_context, std::string_view extension, const cv::Mat &image,
	                 std::vector<uchar> *encoded) -> bool {
		(void)image;
		auto *context = static_cast<WriterCallbackContext *>(raw_context);
		++context->encode_calls;
		context->received_extension = extension;
		if (context->throw_cv_exception) {
			throw cv::Exception(cv::Error::StsError, "fake encoder exception", "fake_encode",
			                    __FILE__, __LINE__);
		}
		if (context->throw_std_exception) {
			throw std::runtime_error("fake encoder exception");
		}
		if (context->create_encoded_output) {
			context->encoded_output = {0x01, 0x02, 0x03};
			encoded->insert(encoded->end(), context->encoded_output.begin(),
			                context->encoded_output.end());
		}
		return context->encode_result;
	}

	auto real_encode(void *raw_context, std::string_view extension, const cv::Mat &image,
	                 std::vector<uchar> *encoded) -> bool {
		auto *context = static_cast<WriterCallbackContext *>(raw_context);
		++context->encode_calls;
		context->received_extension = extension;
		return cv::imencode(std::string(extension), image, *encoded);
	}

	auto fake_dependencies(WriterCallbackContext &context)
	    -> snapshot_internal::SnapshotWriterDependencies {
		return {.context = &context, .encode_image = fake_encode};
	}

	auto real_dependencies(WriterCallbackContext &context)
	    -> snapshot_internal::SnapshotWriterDependencies {
		return {.context = &context, .encode_image = real_encode};
	}

	auto fail_parent_sync(const std::filesystem::path & /*path*/) -> bool {
		return false;
	}

	auto expect_invalid_batch_rejected(const std::string &name, const std::vector<cv::Mat> &frames)
	    -> bool {
		bool                  ok        = true;
		const auto            temp_root = make_temp_root(name, ok);
		const auto            log_root  = temp_root / "log";
		const auto            output    = log_root / "snapshots" / "test.jpg";
		WriterCallbackContext context;
		const bool result = snapshot_internal::write_snapshot_at_path(frames, text_lines(), output,
		                                                              fake_dependencies(context));
		ok &= expect(!result, name + " returns false");
		ok &= expect(context.encode_calls == 0, name + " skips encoder");
		ok &= expect(!fs::exists(log_root), name + " creates no log root");
		return cleanup_temp_root(temp_root, ok);
	}

	auto frame_validation_regressions() -> bool {
		bool ok     = true;
		auto frames = tiny_frames();
		frames[1]   = cv::Mat{};
		ok &= expect_invalid_batch_rejected("empty-member", frames);
		ok &= expect_invalid_batch_rejected("wrong-channel",
		                                    {cv::Mat(8, 8, CV_8UC1, cv::Scalar(10))});
		ok &= expect_invalid_batch_rejected("wrong-pixel-depth",
		                                    {cv::Mat(8, 8, CV_16UC3, cv::Scalar(10, 20, 30))});
		ok &= expect_invalid_batch_rejected("combined-width",
		                                    {cv::Mat(1, 4097, CV_8UC3, cv::Scalar(10, 20, 30)),
		                                     cv::Mat(1, 4097, CV_8UC3, cv::Scalar(40, 50, 60))});
		ok &= expect_invalid_batch_rejected(
		    "oversized-frame",
		    {cv::Mat(howdy::native::kMaxFrameDimension + 1, 1, CV_8UC3, cv::Scalar(10, 20, 30))});
		ok &= expect_invalid_batch_rejected("shape-mismatch",
		                                    {cv::Mat(8, 8, CV_8UC3, cv::Scalar(10, 20, 30)),
		                                     cv::Mat(9, 8, CV_8UC3, cv::Scalar(40, 50, 60))});
		return ok;
	}

	auto missing_encoder_dependency() -> bool {
		bool       ok        = true;
		const auto temp_root = make_temp_root("missing-encoder", ok);
		const auto log_root  = temp_root / "log";
		const auto output    = log_root / "snapshots" / "test.jpg";
		ok &= expect(
		    !snapshot_internal::write_snapshot_at_path(tiny_frames(), text_lines(), output, {}),
		    "missing encoder returns false");
		ok &= expect(!fs::exists(log_root), "missing encoder creates no log root");
		return cleanup_temp_root(temp_root, ok);
	}

	auto encoder_failure_preserves_destination(const std::string &name, bool standard_exception,
	                                           bool opencv_exception) -> bool {
		bool            ok        = true;
		const auto      temp_root = make_temp_root(name, ok);
		const auto      output    = temp_root / "log" / "snapshots" / "test.jpg";
		const auto      original  = std::string("original snapshot contents");
		std::error_code ec;
		fs::create_directories(output.parent_path(), ec);
		ok &= expect(!ec && write_file(output, original), name + " creates destination");
		ok &= expect(chmod(output.c_str(), 0640) == 0, name + " sets destination mode");
		WriterCallbackContext context;
		context.encode_result       = standard_exception || opencv_exception;
		context.throw_std_exception = standard_exception;
		context.throw_cv_exception  = opencv_exception;
		const bool result           = snapshot_internal::write_snapshot_at_path(
		    tiny_frames(), text_lines(), output, fake_dependencies(context));
		ok &= expect(!result, name + " returns false");
		ok &= expect(read_file(output) == original, name + " preserves destination content");
		ok &= expect_mode(output, 0640, name + " preserves destination mode");
		ok &= expect(count_staged_files(output.parent_path()) == 0, name + " removes staged file");
		return cleanup_temp_root(temp_root, ok);
	}

	auto empty_encoder_output_fails_closed() -> bool {
		bool                  ok        = true;
		const auto            temp_root = make_temp_root("empty-encoder-output", ok);
		const auto            output    = temp_root / "log" / "snapshots" / "test.jpg";
		WriterCallbackContext context;
		context.create_encoded_output = false;
		const bool result             = snapshot_internal::write_snapshot_at_path(
		    tiny_frames(), text_lines(), output, fake_dependencies(context));
		ok &= expect(!result, "empty encoder output returns false");
		ok &= expect(!fs::exists(output), "empty encoder output creates no destination");
		ok &= expect(count_staged_files(output.parent_path()) == 0,
		             "empty encoder output removes staged file");
		return cleanup_temp_root(temp_root, ok);
	}

	auto encoded_bytes_install(bool existing_destination) -> bool {
		const auto name =
		    std::string(existing_destination ? "existing-mode-reset" : "encoded-byte-install");
		bool       ok        = true;
		const auto temp_root = make_temp_root(name, ok);
		const auto output    = temp_root / "log" / "snapshots" / "test.jpg";
		if (existing_destination) {
			std::error_code ec;
			fs::create_directories(output.parent_path(), ec);
			ok &= expect(!ec && write_file(output, "old"), name + " creates destination");
			ok &= expect(chmod(output.c_str(), 0640) == 0, name + " sets old mode");
		}
		WriterCallbackContext context;
		const bool            result = snapshot_internal::write_snapshot_at_path(
		    tiny_frames(), text_lines(), output, fake_dependencies(context));
		ok &= expect(result, name + " returns true");
		ok &= expect(read_file(output) == std::string("\x01\x02\x03", 3),
		             name + " installs exact encoded bytes");
		ok &= expect_mode(output, kSnapshotFileMode, name + " output mode is 0600");
		ok &=
		    expect(count_staged_files(output.parent_path()) == 0, name + " leaves no staged file");
		return cleanup_temp_root(temp_root, ok);
	}

	auto directory_target_rejects_before_encoding() -> bool {
		bool            ok        = true;
		const auto      temp_root = make_temp_root("directory-target", ok);
		const auto      output    = temp_root / "log" / "snapshots" / "test.jpg";
		std::error_code ec;
		fs::create_directories(output, ec);
		ok &= expect(!ec, "directory target created");
		WriterCallbackContext context;
		const bool            result = snapshot_internal::write_snapshot_at_path(
		    tiny_frames(), text_lines(), output, fake_dependencies(context));
		ok &= expect(!result, "directory target returns false");
		ok &= expect(context.encode_calls == 0, "directory target skips encoder");
		ok &= expect(fs::is_directory(output), "directory target remains directory");
		ok &= expect(count_staged_files(output.parent_path()) == 0,
		             "directory target creates no stage");
		return cleanup_temp_root(temp_root, ok);
	}

	auto blocked_parent_rejects_before_encoding() -> bool {
		bool       ok        = true;
		const auto temp_root = make_temp_root("blocked-parent", ok);
		const auto blocker   = temp_root / "log";
		const auto output    = blocker / "snapshots" / "test.jpg";
		ok &= expect(write_file(blocker, "blocking content"), "blocked parent creates blocker");
		WriterCallbackContext context;
		std::ostringstream    error;
		{
			StreamRedirect redirect(std::cerr, error.rdbuf());
			ok &= expect(!snapshot_internal::write_snapshot_at_path(
			                 tiny_frames(), text_lines(), output, fake_dependencies(context)),
			             "blocked parent returns false");
		}
		ok &= expect(context.encode_calls == 0, "blocked parent skips encoder");
		ok &= expect(read_file(blocker) == "blocking content", "blocked parent preserves blocker");
		return cleanup_temp_root(temp_root, ok);
	}

	auto real_image_output(const std::string &name, const std::string &filename,
	                       const std::string &extension) -> bool {
		bool                  ok        = true;
		const auto            temp_root = make_temp_root(name, ok);
		const auto            output    = temp_root / "log" / "snapshots" / filename;
		WriterCallbackContext context;
		const bool            result = snapshot_internal::write_snapshot_at_path(
		    tiny_frames(), text_lines(), output, real_dependencies(context));
		ok &= expect(result, name + " returns true");
		ok &= expect(!cv::imread(output.string()).empty(), name + " decodes successfully");
		ok &= expect(context.received_extension == extension, name + " receives exact extension");
		ok &= expect_mode(output, kSnapshotFileMode, name + " output mode is 0600");
		return cleanup_temp_root(temp_root, ok);
	}

	auto extensionless_rejects_before_directory_setup() -> bool {
		bool                  ok        = true;
		const auto            temp_root = make_temp_root("extensionless", ok);
		const auto            log_root  = temp_root / "log";
		const auto            output    = log_root / "snapshots" / "snapshot";
		WriterCallbackContext context;
		const bool            result = snapshot_internal::write_snapshot_at_path(
		    tiny_frames(), text_lines(), output, fake_dependencies(context));
		ok &= expect(!result, "extensionless output returns false");
		ok &= expect(context.encode_calls == 0, "extensionless output skips encoder");
		ok &= expect(!fs::exists(log_root),
		             "extensionless output intentionally skips directory setup");
		ok &= expect(!fs::exists(output), "extensionless output creates no destination");
		ok &= expect(count_staged_files(output.parent_path()) == 0,
		             "extensionless output creates no staged file");
		return cleanup_temp_root(temp_root, ok);
	}

	auto insecure_existing_log_root_rejects_before_encoding() -> bool {
		bool            ok        = true;
		const auto      temp_root = make_temp_root("insecure-log-root", ok);
		const auto      log_root  = temp_root / "log";
		const auto      output    = log_root / "snapshots" / "test.jpg";
		std::error_code ec;
		fs::create_directories(log_root, ec);
		ok &= expect(!ec, "insecure log root created");
		ok &= expect(chmod(log_root.c_str(), S_IRWXU | S_IRWXG) == 0,
		             "insecure log root made group-writable");
		WriterCallbackContext context;
		std::ostringstream    error;
		{
			StreamRedirect redirect(std::cerr, error.rdbuf());
			ok &= expect(!snapshot_internal::write_snapshot_at_path(
			                 tiny_frames(), text_lines(), output, fake_dependencies(context)),
			             "insecure log root returns false");
		}
		ok &= expect(context.encode_calls == 0, "insecure log root skips encoder");
		ok &= expect(!fs::exists(output), "insecure log root creates no output");
		ok &= expect(error.str().contains("Log directory must not be group-writable"),
		             "insecure log root reports security diagnostic");
		return cleanup_temp_root(temp_root, ok);
	}

	auto missing_directories_are_created_and_secured() -> bool {
		bool                  ok            = true;
		const auto            temp_root     = make_temp_root("directory-creation", ok);
		const auto            log_root      = temp_root / "log";
		const auto            snapshots_dir = log_root / "snapshots";
		const auto            output        = snapshots_dir / "test.jpg";
		WriterCallbackContext context;
		const bool            result = snapshot_internal::write_snapshot_at_path(
		    tiny_frames(), text_lines(), output, fake_dependencies(context));
		ok &= expect(result, "directory creation returns true");
		ok &= expect(context.encode_calls == 1, "directory creation calls encoder once");
		ok &= expect_mode(log_root, kSnapshotDirectoryMode, "created log root mode is 0750");
		ok &= expect_mode(snapshots_dir, kSnapshotDirectoryMode,
		                  "created snapshot directory mode is 0750");
		ok &= expect(fs::exists(output), "directory creation creates output");
		return cleanup_temp_root(temp_root, ok);
	}

	auto mixed_width_frames_are_supported() -> bool {
		bool                  ok        = true;
		const auto            temp_root = make_temp_root("mixed-width", ok);
		const auto            output    = temp_root / "log" / "snapshots" / "test.jpg";
		WriterCallbackContext context;
		const bool            result = snapshot_internal::write_snapshot_at_path(
		    {cv::Mat(8, 4, CV_8UC3, cv::Scalar(10, 20, 30)),
		     cv::Mat(8, 12, CV_8UC3, cv::Scalar(40, 50, 60))},
		    text_lines(), output, fake_dependencies(context));
		ok &= expect(result, "mixed-width frames return true");
		ok &= expect(context.encode_calls == 1, "mixed-width frames call encoder once");
		return cleanup_temp_root(temp_root, ok);
	}

	auto committed_sync_failure_is_reported() -> bool {
		bool                  ok        = true;
		const auto            temp_root = make_temp_root("committed-sync-failure", ok);
		const auto            output    = temp_root / "log" / "snapshots" / "test.jpg";
		WriterCallbackContext context;
		auto                  dependencies = fake_dependencies(context);
		dependencies.sync_parent           = fail_parent_sync;
		howdy::native::AtomicFileCommitResult commit_result;
		const bool                            result = snapshot_internal::write_snapshot_at_path(
		    tiny_frames(), text_lines(), output, dependencies, &commit_result);
		ok &= expect(!result, "committed sync failure returns false");
		ok &= expect(commit_result == howdy::native::AtomicFileCommitResult::kCommittedSyncFailed,
		             "committed sync failure preserves exact commit state");
		ok &= expect(fs::exists(output), "committed sync failure leaves snapshot visible");
		return cleanup_temp_root(temp_root, ok);
	}

}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= frame_validation_regressions();
	ok &= missing_encoder_dependency();
	ok &= encoder_failure_preserves_destination("encoder-false", false, false);
	ok &= encoder_failure_preserves_destination("encoder-std-exception", true, false);
	ok &= encoder_failure_preserves_destination("encoder-cv-exception", false, true);
	ok &= empty_encoder_output_fails_closed();
	ok &= encoded_bytes_install(false);
	ok &= encoded_bytes_install(true);
	ok &= directory_target_rejects_before_encoding();
	ok &= blocked_parent_rejects_before_encoding();
	ok &= real_image_output("jpeg-output", "test.jpg", ".jpg");
	ok &= real_image_output("png-output", "test.png", ".png");
	ok &= real_image_output("hidden-png-output", ".png", ".png");
	ok &= extensionless_rejects_before_directory_setup();
	ok &= insecure_existing_log_root_rejects_before_encoding();
	ok &= missing_directories_are_created_and_secured();
	ok &= mixed_width_frames_are_supported();
	ok &= committed_sync_failure_is_reported();
	return ok ? 0 : 1;
}
