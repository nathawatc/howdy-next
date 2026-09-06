#include "cli/snapshot/internal.hpp"
#include "test_support.hpp"
#include "vision/frame_validation.hpp"

#include <array>
#include <cerrno>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <sys/stat.h>
#include <sys/wait.h>

namespace {

	using howdy::test::expect;
	using howdy::test::read_file;
	using howdy::test::write_file;

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

	struct ConcurrentWriterContext {
		int                  ready_fd   = -1;
		int                  release_fd = -1;
		bool                 wait_once  = true;
		std::array<uchar, 3> encoded_output{};
	};

	auto write_pipe_byte(int fd, char byte) -> bool {
		while (true) {
			const auto result = write(fd, &byte, 1);
			if (result == 1) {
				return true;
			}
			if (result < 0 && errno == EINTR) {
				continue;
			}
			return false;
		}
	}

	auto read_pipe_byte(int fd, char *byte) -> bool {
		while (true) {
			const auto result = read(fd, byte, 1);
			if (result == 1) {
				return true;
			}
			if (result < 0 && errno == EINTR) {
				continue;
			}
			return false;
		}
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

	auto candidate_path_for_test(const fs::path &base, std::size_t collision_index) -> fs::path {
		if (collision_index == 0) {
			return base;
		}
		return base.parent_path() / (base.stem().string() + "-" + std::to_string(collision_index) +
		                             base.extension().string());
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

	auto concurrent_encode(void *raw_context, std::string_view /*extension*/,
	                       const cv::Mat & /*image*/, std::vector<uchar> *encoded) -> bool {
		auto *context = static_cast<ConcurrentWriterContext *>(raw_context);
		if (context->wait_once) {
			if (!write_pipe_byte(context->ready_fd, 'r')) {
				return false;
			}
			char release = 0;
			if (!read_pipe_byte(context->release_fd, &release)) {
				return false;
			}
			context->wait_once = false;
		}
		encoded->insert(encoded->end(), context->encoded_output.begin(),
		                context->encoded_output.end());
		return true;
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

	auto encoded_bytes_install() -> bool {
		bool                  ok        = true;
		const auto            temp_root = make_temp_root("encoded-byte-install", ok);
		const auto            output    = temp_root / "log" / "snapshots" / "test.jpg";
		WriterCallbackContext context;
		const bool            result = snapshot_internal::write_snapshot_at_path(
		    tiny_frames(), text_lines(), output, fake_dependencies(context));
		ok &= expect(result, "encoded-byte-install returns true");
		ok &= expect(read_file(output) == std::string("\x01\x02\x03", 3),
		             "encoded-byte-install installs exact encoded bytes");
		ok &= expect_mode(output, kSnapshotFileMode, "encoded-byte-install output mode is 0600");
		ok &= expect(count_staged_files(output.parent_path()) == 0,
		             "encoded-byte-install leaves no staged file");
		return cleanup_temp_root(temp_root, ok);
	}

	auto existing_destination_is_not_replaced() -> bool {
		bool              ok        = true;
		const auto        temp_root = make_temp_root("existing-destination", ok);
		const auto        output    = temp_root / "log" / "snapshots" / "test.jpg";
		const std::string original  = "old snapshot";
		std::error_code   ec;
		ok &= expect(fs::create_directories(output.parent_path(), ec) && !ec,
		             "existing destination creates parent");
		ok &= expect(write_file(output, original), "existing destination writes original");
		ok &= expect(chmod(output.c_str(), 0640) == 0, "existing destination sets original mode");
		WriterCallbackContext                 context;
		howdy::native::AtomicFileCommitResult commit_result;
		const bool                            result = snapshot_internal::write_snapshot_at_path(
		    tiny_frames(), text_lines(), output, fake_dependencies(context), &commit_result);
		ok &= expect(!result, "existing destination returns false");
		ok &= expect(commit_result == howdy::native::AtomicFileCommitResult::kDestinationExists,
		             "existing destination reports collision");
		ok &= expect(!howdy::native::atomic_file_may_have_committed(commit_result),
		             "existing destination is not possibly committed");
		ok &= expect(read_file(output) == original, "existing destination content is unchanged");
		ok &= expect_mode(output, 0640, "existing destination mode is unchanged");
		ok &= expect(count_staged_files(output.parent_path()) == 0,
		             "existing destination removes staged file");
		return cleanup_temp_root(temp_root, ok);
	}

	auto unique_path_install_selects_next_candidate() -> bool {
		bool            ok        = true;
		const auto      temp_root = make_temp_root("unique-path-collisions", ok);
		const auto      base      = temp_root / "log" / "snapshots" / "20260816T100012.jpg";
		std::error_code ec;
		ok &= expect(fs::create_directories(base.parent_path(), ec) && !ec,
		             "unique path creates parent");
		const std::array existing = {
		    std::pair{base, std::string("base snapshot")},
		    std::pair{base.parent_path() / "20260816T100012-1.jpg", std::string("first snapshot")},
		    std::pair{base.parent_path() / "20260816T100012-2.jpg", std::string("second snapshot")},
		};
		for (const auto &[path, contents] : existing) {
			ok &= expect(write_file(path, contents), "unique path writes existing candidate");
		}
		WriterCallbackContext                 context;
		howdy::native::AtomicFileCommitResult commit_result;
		const auto installed = snapshot_internal::write_snapshot_with_unique_path(
		    tiny_frames(), text_lines(), base, fake_dependencies(context), &commit_result);
		const auto expected = base.parent_path() / "20260816T100012-3.jpg";
		ok &= expect(installed == expected, "unique path selects next available suffix");
		ok &= expect(commit_result == howdy::native::AtomicFileCommitResult::kCommitted,
		             "unique path reports durable commit");
		ok &= expect(context.encode_calls == 4, "unique path encodes each collision candidate");
		ok &= expect(read_file(expected) == std::string("\x01\x02\x03", 3),
		             "unique path installs new encoded bytes");
		for (const auto &[path, contents] : existing) {
			ok &= expect(read_file(path) == contents, "unique path preserves existing candidate");
		}
		ok &= expect_mode(expected, kSnapshotFileMode, "unique path output mode is 0600");
		ok &= expect(count_staged_files(base.parent_path()) == 0,
		             "unique path leaves no staged file");
		return cleanup_temp_root(temp_root, ok);
	}

	auto unique_path_collision_exhaustion() -> bool {
		bool            ok        = true;
		const auto      temp_root = make_temp_root("unique-path-exhaustion", ok);
		const auto      base      = temp_root / "log" / "snapshots" / "20260816T100012.jpg";
		std::error_code ec;
		ok &= expect(fs::create_directories(base.parent_path(), ec) && !ec,
		             "collision exhaustion creates parent");
		for (std::size_t index = 0; index < snapshot_internal::kMaxSnapshotNameAttempts; ++index) {
			ok &= expect(write_file(candidate_path_for_test(base, index),
			                        "occupied-" + std::to_string(index)),
			             "collision exhaustion occupies candidate");
		}
		WriterCallbackContext                 context;
		howdy::native::AtomicFileCommitResult commit_result;
		std::ostringstream                    error;
		std::filesystem::path                 installed;
		{
			StreamRedirect redirect(std::cerr, error.rdbuf());
			installed = snapshot_internal::write_snapshot_with_unique_path(
			    tiny_frames(), text_lines(), base, fake_dependencies(context), &commit_result);
		}
		ok &= expect(installed.empty(), "collision exhaustion returns no path");
		ok &= expect(commit_result == howdy::native::AtomicFileCommitResult::kDestinationExists,
		             "collision exhaustion reports final collision");
		ok &= expect(!howdy::native::atomic_file_may_have_committed(commit_result),
		             "collision exhaustion is not possibly committed");
		ok &= expect(error.str().contains("Could not allocate unique snapshot filename"),
		             "collision exhaustion reports bounded failure");
		for (std::size_t index = 0; index < snapshot_internal::kMaxSnapshotNameAttempts; ++index) {
			ok &= expect(read_file(candidate_path_for_test(base, index)) ==
			                 "occupied-" + std::to_string(index),
			             "collision exhaustion preserves existing candidate");
		}
		ok &= expect(count_staged_files(base.parent_path()) == 0,
		             "collision exhaustion leaves no staged file");
		return cleanup_temp_root(temp_root, ok);
	}

	auto close_pipe(std::array<int, 2> &pipe_fds) -> void {
		for (auto &fd : pipe_fds) {
			if (fd >= 0) {
				close(fd);
				fd = -1;
			}
		}
	}

	auto reap_children(const std::array<pid_t, 2> &children, std::size_t child_count,
	                   bool terminate) -> bool {
		if (terminate) {
			for (std::size_t index = 0; index < child_count; ++index) {
				kill(children[index], SIGKILL);
			}
		}
		bool ok = true;
		for (std::size_t index = 0; index < child_count; ++index) {
			int   status = 0;
			pid_t result = -1;
			do {
				result = waitpid(children[index], &status, 0);
			} while (result < 0 && errno == EINTR);
			ok &= result == children[index] && WIFEXITED(status) && WEXITSTATUS(status) == 0;
		}
		return ok;
	}

	auto run_concurrent_writer_child(const fs::path &base, const std::array<int, 2> &ready_pipe,
	                                 const std::array<int, 2> &release_pipe, std::size_t index)
	    -> void {
		close(ready_pipe[0]);
		close(release_pipe[1]);
		ConcurrentWriterContext context{
		    .ready_fd       = ready_pipe[1],
		    .release_fd     = release_pipe[0],
		    .wait_once      = true,
		    .encoded_output = index == 0 ? std::array<uchar, 3>{'A', 'A', 'A'}
		                                 : std::array<uchar, 3>{'B', 'B', 'B'},
		};
		const auto dependencies = snapshot_internal::SnapshotWriterDependencies{
		    .context = &context, .encode_image = concurrent_encode};
		const auto installed = snapshot_internal::write_snapshot_with_unique_path(
		    tiny_frames(), text_lines(), base, dependencies);
		close(ready_pipe[1]);
		close(release_pipe[0]);
		_exit(installed.empty() ? 1 : 0);
	}

	auto run_concurrent_writers(const fs::path &base) -> bool {
		std::array<int, 2> ready_pipe{{-1, -1}};
		std::array<int, 2> release_pipe{{-1, -1}};
		const bool         ready_created   = pipe(ready_pipe.data()) == 0;
		const bool         release_created = ready_created && pipe(release_pipe.data()) == 0;
		if (!ready_created || !release_created) {
			close_pipe(ready_pipe);
			close_pipe(release_pipe);
			return false;
		}

		std::array<pid_t, 2> children{{-1, -1}};
		std::size_t          child_count = 0;
		for (; child_count < children.size(); ++child_count) {
			children[child_count] = fork();
			if (children[child_count] < 0) {
				close_pipe(ready_pipe);
				close_pipe(release_pipe);
				return reap_children(children, child_count, true);
			}
			if (children[child_count] == 0) {
				run_concurrent_writer_child(base, ready_pipe, release_pipe, child_count);
			}
		}

		close(ready_pipe[1]);
		ready_pipe[1] = -1;
		close(release_pipe[0]);
		release_pipe[0]  = -1;
		char       ready = 0;
		const bool barriers_ready =
		    read_pipe_byte(ready_pipe[0], &ready) && read_pipe_byte(ready_pipe[0], &ready);
		close(ready_pipe[0]);
		ready_pipe[0]       = -1;
		const bool released = barriers_ready && write_pipe_byte(release_pipe[1], 'g') &&
		                      write_pipe_byte(release_pipe[1], 'g');
		close(release_pipe[1]);
		release_pipe[1] = -1;
		const bool children_succeeded =
		    reap_children(children, children.size(), !barriers_ready || !released);
		return barriers_ready && released && children_succeeded;
	}

	auto concurrent_unique_path_install() -> bool {
		bool            ok        = true;
		const auto      temp_root = make_temp_root("concurrent-unique-path", ok);
		const auto      base      = temp_root / "log" / "snapshots" / "20260816T100012.jpg";
		std::error_code ec;
		ok &= expect(fs::create_directories(base.parent_path(), ec) && !ec,
		             "concurrent install creates parent");
		ok &= expect(write_file(base, "existing snapshot"), "concurrent install creates base");
		ok &=
		    expect(chmod(base.c_str(), kSnapshotFileMode) == 0, "concurrent install secures base");
		if (!ok) {
			return cleanup_temp_root(temp_root, false);
		}
		ok &= expect(run_concurrent_writers(base), "concurrent install writers succeed");

		const auto first           = candidate_path_for_test(base, 1);
		const auto second          = candidate_path_for_test(base, 2);
		const auto first_contents  = read_file(first);
		const auto second_contents = read_file(second);
		ok &= expect(read_file(base) == "existing snapshot",
		             "concurrent install preserves existing base");
		ok &= expect((first_contents == "AAA" && second_contents == "BBB") ||
		                 (first_contents == "BBB" && second_contents == "AAA"),
		             "concurrent install preserves separate writer contents");
		ok &= expect(count_staged_files(base.parent_path()) == 0,
		             "concurrent install leaves no staged files");
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

	struct ImageOutputCase {
		std::string name;
		std::string filename;
		std::string extension;
	};

	auto real_image_output(const ImageOutputCase &test_case) -> bool {
		bool                  ok        = true;
		const auto            temp_root = make_temp_root(test_case.name, ok);
		const auto            output    = temp_root / "log" / "snapshots" / test_case.filename;
		WriterCallbackContext context;
		const bool            result = snapshot_internal::write_snapshot_at_path(
		    tiny_frames(), text_lines(), output, real_dependencies(context));
		ok &= expect(result, test_case.name + " returns true");
		ok &=
		    expect(!cv::imread(output.string()).empty(), test_case.name + " decodes successfully");
		ok &= expect(context.received_extension == test_case.extension,
		             test_case.name + " receives exact extension");
		ok &= expect_mode(output, kSnapshotFileMode, test_case.name + " output mode is 0600");
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
	ok &= encoded_bytes_install();
	ok &= existing_destination_is_not_replaced();
	ok &= unique_path_install_selects_next_candidate();
	ok &= unique_path_collision_exhaustion();
	ok &= concurrent_unique_path_install();
	ok &= directory_target_rejects_before_encoding();
	ok &= blocked_parent_rejects_before_encoding();
	ok &= real_image_output({.name = "jpeg-output", .filename = "test.jpg", .extension = ".jpg"});
	ok &= real_image_output({.name = "png-output", .filename = "test.png", .extension = ".png"});
	ok &= real_image_output({.name = "hidden-png-output", .filename = ".png", .extension = ".png"});
	ok &= extensionless_rejects_before_directory_setup();
	ok &= insecure_existing_log_root_rejects_before_encoding();
	ok &= missing_directories_are_created_and_secured();
	ok &= mixed_width_frames_are_supported();
	ok &= committed_sync_failure_is_reported();
	return ok ? 0 : 1;
}
