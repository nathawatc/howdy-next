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

	using howdy::test::Expect;
	using howdy::test::ReadFile;
	using howdy::test::WriteFile;

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

	auto WritePipeByte(int fd, char byte) -> bool {
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

	auto ReadPipeByte(int fd, char *byte) -> bool {
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

	auto MakeTempRoot(const std::string &name, bool &ok) -> fs::path {
		const auto root =
		    fs::current_path() / ("howdy-snapshot-writer-" + name + "-" + std::to_string(getpid()));
		std::error_code ec;
		fs::remove_all(root, ec);
		ok &= Expect(!ec, "remove stale temp root for " + name);
		fs::create_directories(root, ec);
		ok &= Expect(!ec, "create temp root for " + name);
		ok &= Expect(chmod(root.c_str(), kSnapshotDirectoryMode) == 0,
		             "secure temp root for " + name);
		return root;
	}

	auto CleanupTempRoot(const fs::path &root, bool ok) -> bool {
		if (!ok) {
			std::cerr << "Leaving snapshot writer test artifacts: " << root << "\n";
			return false;
		}
		std::error_code ec;
		fs::remove_all(root, ec);
		return Expect(!ec, "remove temp root " + root.string());
	}

	auto PathMode(const fs::path &path) -> std::optional<mode_t> {
		struct stat file_stat{};
		if (stat(path.c_str(), &file_stat) != 0) {
			return std::nullopt;
		}
		return file_stat.st_mode & 0777;
	}

	auto ExpectMode(const fs::path &path, mode_t expected_mode, const std::string &message)
	    -> bool {
		const auto mode = PathMode(path);
		return Expect(mode.has_value() && *mode == expected_mode, message);
	}

	auto CountStagedFiles(const fs::path &directory) -> std::size_t {
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

	auto TinyFrames() -> std::vector<cv::Mat> {
		return {
		    cv::Mat(8, 8, CV_8UC3, cv::Scalar(10, 20, 30)),
		    cv::Mat(8, 8, CV_8UC3, cv::Scalar(40, 50, 60)),
		    cv::Mat(8, 8, CV_8UC3, cv::Scalar(70, 80, 90)),
		    cv::Mat(8, 8, CV_8UC3, cv::Scalar(100, 110, 120)),
		};
	}

	auto TextLines() -> std::vector<std::string> {
		return {"snapshot writer test"};
	}

	auto CandidatePathForTest(const fs::path &base, std::size_t collision_index) -> fs::path {
		if (collision_index == 0) {
			return base;
		}
		return base.parent_path() / (base.stem().string() + "-" + std::to_string(collision_index) +
		                             base.extension().string());
	}

	auto FakeEncode(void *raw_context, std::string_view extension, const cv::Mat &image,
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

	auto ConcurrentEncode(void *raw_context, std::string_view /*extension*/,
	                      const cv::Mat & /*image*/, std::vector<uchar> *encoded) -> bool {
		auto *context = static_cast<ConcurrentWriterContext *>(raw_context);
		if (context->wait_once) {
			if (!WritePipeByte(context->ready_fd, 'r')) {
				return false;
			}
			char release = 0;
			if (!ReadPipeByte(context->release_fd, &release)) {
				return false;
			}
			context->wait_once = false;
		}
		encoded->insert(encoded->end(), context->encoded_output.begin(),
		                context->encoded_output.end());
		return true;
	}

	auto RealEncode(void *raw_context, std::string_view extension, const cv::Mat &image,
	                std::vector<uchar> *encoded) -> bool {
		auto *context = static_cast<WriterCallbackContext *>(raw_context);
		++context->encode_calls;
		context->received_extension = extension;
		return cv::imencode(std::string(extension), image, *encoded);
	}

	auto FakeDependencies(WriterCallbackContext &context, const fs::path &root)
	    -> snapshot_internal::SnapshotWriterDependencies {
		return {.context = &context, .encode_image = FakeEncode, .validation_root = {root}};
	}

	auto RealDependencies(WriterCallbackContext &context, const fs::path &root)
	    -> snapshot_internal::SnapshotWriterDependencies {
		return {.context = &context, .encode_image = RealEncode, .validation_root = {root}};
	}

	auto FailParentSync(const std::filesystem::path & /*path*/) -> bool {
		return false;
	}

	auto ExpectInvalidBatchRejected(const std::string &name, const std::vector<cv::Mat> &frames)
	    -> bool {
		bool                  ok        = true;
		const auto            temp_root = MakeTempRoot(name, ok);
		const auto            log_root  = temp_root / "log";
		const auto            output    = log_root / "snapshots" / "test.jpg";
		WriterCallbackContext context;
		const bool            result = snapshot_internal::WriteSnapshotAtPath(
		    frames, TextLines(), output, FakeDependencies(context, temp_root));
		ok &= Expect(!result, name + " returns false");
		ok &= Expect(context.encode_calls == 0, name + " skips encoder");
		ok &= Expect(!fs::exists(log_root), name + " creates no log root");
		return CleanupTempRoot(temp_root, ok);
	}

	auto FrameValidationRegressions() -> bool {
		bool ok     = true;
		auto frames = TinyFrames();
		frames[1]   = cv::Mat{};
		ok &= ExpectInvalidBatchRejected("empty-member", frames);
		ok &= ExpectInvalidBatchRejected("wrong-channel", {cv::Mat(8, 8, CV_8UC1, cv::Scalar(10))});
		ok &= ExpectInvalidBatchRejected("wrong-pixel-depth",
		                                 {cv::Mat(8, 8, CV_16UC3, cv::Scalar(10, 20, 30))});
		ok &= ExpectInvalidBatchRejected("combined-width",
		                                 {cv::Mat(1, 4097, CV_8UC3, cv::Scalar(10, 20, 30)),
		                                  cv::Mat(1, 4097, CV_8UC3, cv::Scalar(40, 50, 60))});
		ok &= ExpectInvalidBatchRejected(
		    "oversized-frame",
		    {cv::Mat(howdy::native::kMaxFrameDimension + 1, 1, CV_8UC3, cv::Scalar(10, 20, 30))});
		ok &= ExpectInvalidBatchRejected("shape-mismatch",
		                                 {cv::Mat(8, 8, CV_8UC3, cv::Scalar(10, 20, 30)),
		                                  cv::Mat(9, 8, CV_8UC3, cv::Scalar(40, 50, 60))});
		return ok;
	}

	auto MissingEncoderDependency() -> bool {
		bool       ok        = true;
		const auto temp_root = MakeTempRoot("missing-encoder", ok);
		const auto log_root  = temp_root / "log";
		const auto output    = log_root / "snapshots" / "test.jpg";
		ok &= Expect(!snapshot_internal::WriteSnapshotAtPath(TinyFrames(), TextLines(), output, {}),
		             "missing encoder returns false");
		ok &= Expect(!fs::exists(log_root), "missing encoder creates no log root");
		return CleanupTempRoot(temp_root, ok);
	}

	auto EncoderFailurePreservesDestination(const std::string &name, bool standard_exception,
	                                        bool opencv_exception) -> bool {
		bool            ok        = true;
		const auto      temp_root = MakeTempRoot(name, ok);
		const auto      output    = temp_root / "log" / "snapshots" / "test.jpg";
		const auto      original  = std::string("original snapshot contents");
		std::error_code ec;
		fs::create_directories(output.parent_path(), ec);
		ok &= Expect(!ec && WriteFile(output, original), name + " creates destination");
		ok &= Expect(chmod(output.c_str(), 0640) == 0, name + " sets destination mode");
		WriterCallbackContext context;
		context.encode_result       = standard_exception || opencv_exception;
		context.throw_std_exception = standard_exception;
		context.throw_cv_exception  = opencv_exception;
		const bool result           = snapshot_internal::WriteSnapshotAtPath(
		    TinyFrames(), TextLines(), output, FakeDependencies(context, temp_root));
		ok &= Expect(!result, name + " returns false");
		ok &= Expect(ReadFile(output) == original, name + " preserves destination content");
		ok &= ExpectMode(output, 0640, name + " preserves destination mode");
		ok &= Expect(CountStagedFiles(output.parent_path()) == 0, name + " removes staged file");
		return CleanupTempRoot(temp_root, ok);
	}

	auto EmptyEncoderOutputFailsClosed() -> bool {
		bool                  ok        = true;
		const auto            temp_root = MakeTempRoot("empty-encoder-output", ok);
		const auto            output    = temp_root / "log" / "snapshots" / "test.jpg";
		WriterCallbackContext context;
		context.create_encoded_output = false;
		const bool result             = snapshot_internal::WriteSnapshotAtPath(
		    TinyFrames(), TextLines(), output, FakeDependencies(context, temp_root));
		ok &= Expect(!result, "empty encoder output returns false");
		ok &= Expect(!fs::exists(output), "empty encoder output creates no destination");
		ok &= Expect(CountStagedFiles(output.parent_path()) == 0,
		             "empty encoder output removes staged file");
		return CleanupTempRoot(temp_root, ok);
	}

	auto EncodedBytesInstall() -> bool {
		bool                  ok        = true;
		const auto            temp_root = MakeTempRoot("encoded-byte-install", ok);
		const auto            output    = temp_root / "log" / "snapshots" / "test.jpg";
		WriterCallbackContext context;
		const bool            result = snapshot_internal::WriteSnapshotAtPath(
		    TinyFrames(), TextLines(), output, FakeDependencies(context, temp_root));
		ok &= Expect(result, "encoded-byte-install returns true");
		ok &= Expect(ReadFile(output) == std::string("\x01\x02\x03", 3),
		             "encoded-byte-install installs exact encoded bytes");
		ok &= ExpectMode(output, kSnapshotFileMode, "encoded-byte-install output mode is 0600");
		ok &= Expect(CountStagedFiles(output.parent_path()) == 0,
		             "encoded-byte-install leaves no staged file");
		return CleanupTempRoot(temp_root, ok);
	}

	auto ExistingDestinationIsNotReplaced() -> bool {
		bool              ok        = true;
		const auto        temp_root = MakeTempRoot("existing-destination", ok);
		const auto        output    = temp_root / "log" / "snapshots" / "test.jpg";
		const std::string original  = "old snapshot";
		std::error_code   ec;
		ok &= Expect(fs::create_directories(output.parent_path(), ec) && !ec,
		             "existing destination creates parent");
		ok &= Expect(WriteFile(output, original), "existing destination writes original");
		ok &= Expect(chmod(output.c_str(), 0640) == 0, "existing destination sets original mode");
		WriterCallbackContext                 context;
		howdy::native::AtomicFileCommitResult commit_result;
		const bool                            result = snapshot_internal::WriteSnapshotAtPath(
		    TinyFrames(), TextLines(), output, FakeDependencies(context, temp_root),
		    &commit_result);
		ok &= Expect(!result, "existing destination returns false");
		ok &= Expect(commit_result == howdy::native::AtomicFileCommitResult::kDestinationExists,
		             "existing destination reports collision");
		ok &= Expect(!howdy::native::AtomicFileMayHaveCommitted(commit_result),
		             "existing destination is not possibly committed");
		ok &= Expect(ReadFile(output) == original, "existing destination content is unchanged");
		ok &= ExpectMode(output, 0640, "existing destination mode is unchanged");
		ok &= Expect(CountStagedFiles(output.parent_path()) == 0,
		             "existing destination removes staged file");
		return CleanupTempRoot(temp_root, ok);
	}

	auto UniquePathInstallSelectsNextCandidate() -> bool {
		bool            ok        = true;
		const auto      temp_root = MakeTempRoot("unique-path-collisions", ok);
		const auto      base      = temp_root / "log" / "snapshots" / "20260816T100012.jpg";
		std::error_code ec;
		ok &= Expect(fs::create_directories(base.parent_path(), ec) && !ec,
		             "unique path creates parent");
		const std::array existing = {
		    std::pair{base, std::string("base snapshot")},
		    std::pair{base.parent_path() / "20260816T100012-1.jpg", std::string("first snapshot")},
		    std::pair{base.parent_path() / "20260816T100012-2.jpg", std::string("second snapshot")},
		};
		for (const auto &[path, contents] : existing) {
			ok &= Expect(WriteFile(path, contents), "unique path writes existing candidate");
		}
		WriterCallbackContext                 context;
		howdy::native::AtomicFileCommitResult commit_result;
		const auto installed = snapshot_internal::WriteSnapshotWithUniquePath(
		    TinyFrames(), TextLines(), base, FakeDependencies(context, temp_root), &commit_result);
		const auto expected = base.parent_path() / "20260816T100012-3.jpg";
		ok &= Expect(installed == expected, "unique path selects next available suffix");
		ok &= Expect(commit_result == howdy::native::AtomicFileCommitResult::kCommitted,
		             "unique path reports durable commit");
		ok &= Expect(context.encode_calls == 4, "unique path encodes each collision candidate");
		ok &= Expect(ReadFile(expected) == std::string("\x01\x02\x03", 3),
		             "unique path installs new encoded bytes");
		for (const auto &[path, contents] : existing) {
			ok &= Expect(ReadFile(path) == contents, "unique path preserves existing candidate");
		}
		ok &= ExpectMode(expected, kSnapshotFileMode, "unique path output mode is 0600");
		ok &=
		    Expect(CountStagedFiles(base.parent_path()) == 0, "unique path leaves no staged file");
		return CleanupTempRoot(temp_root, ok);
	}

	auto UniquePathCollisionExhaustion() -> bool {
		bool            ok        = true;
		const auto      temp_root = MakeTempRoot("unique-path-exhaustion", ok);
		const auto      base      = temp_root / "log" / "snapshots" / "20260816T100012.jpg";
		std::error_code ec;
		ok &= Expect(fs::create_directories(base.parent_path(), ec) && !ec,
		             "collision exhaustion creates parent");
		for (std::size_t index = 0; index < snapshot_internal::kMaxSnapshotNameAttempts; ++index) {
			ok &= Expect(
			    WriteFile(CandidatePathForTest(base, index), "occupied-" + std::to_string(index)),
			    "collision exhaustion occupies candidate");
		}
		WriterCallbackContext                 context;
		howdy::native::AtomicFileCommitResult commit_result;
		std::ostringstream                    error;
		std::filesystem::path                 installed;
		{
			StreamRedirect redirect(std::cerr, error.rdbuf());
			installed = snapshot_internal::WriteSnapshotWithUniquePath(
			    TinyFrames(), TextLines(), base, FakeDependencies(context, temp_root),
			    &commit_result);
		}
		ok &= Expect(installed.empty(), "collision exhaustion returns no path");
		ok &= Expect(commit_result == howdy::native::AtomicFileCommitResult::kDestinationExists,
		             "collision exhaustion reports final collision");
		ok &= Expect(!howdy::native::AtomicFileMayHaveCommitted(commit_result),
		             "collision exhaustion is not possibly committed");
		ok &= Expect(error.str().contains("Could not allocate unique snapshot filename"),
		             "collision exhaustion reports bounded failure");
		for (std::size_t index = 0; index < snapshot_internal::kMaxSnapshotNameAttempts; ++index) {
			ok &= Expect(ReadFile(CandidatePathForTest(base, index)) ==
			                 "occupied-" + std::to_string(index),
			             "collision exhaustion preserves existing candidate");
		}
		ok &= Expect(CountStagedFiles(base.parent_path()) == 0,
		             "collision exhaustion leaves no staged file");
		return CleanupTempRoot(temp_root, ok);
	}

	auto ClosePipe(std::array<int, 2> &pipe_fds) -> void {
		for (auto &fd : pipe_fds) {
			if (fd >= 0) {
				close(fd);
				fd = -1;
			}
		}
	}

	auto ReapChildren(const std::array<pid_t, 2> &children, std::size_t child_count, bool terminate)
	    -> bool {
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

	auto RunConcurrentWriterChild(const fs::path &base, const std::array<int, 2> &ready_pipe,
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
		    .context         = &context,
		    .encode_image    = ConcurrentEncode,
		    .validation_root = {base.parent_path().parent_path().parent_path()}};
		const auto installed = snapshot_internal::WriteSnapshotWithUniquePath(
		    TinyFrames(), TextLines(), base, dependencies);
		close(ready_pipe[1]);
		close(release_pipe[0]);
		_exit(installed.empty() ? 1 : 0);
	}

	auto RunConcurrentWriters(const fs::path &base) -> bool {
		std::array<int, 2> ready_pipe{{-1, -1}};
		std::array<int, 2> release_pipe{{-1, -1}};
		const bool         ready_created   = pipe(ready_pipe.data()) == 0;
		const bool         release_created = ready_created && pipe(release_pipe.data()) == 0;
		if (!ready_created || !release_created) {
			ClosePipe(ready_pipe);
			ClosePipe(release_pipe);
			return false;
		}

		std::array<pid_t, 2> children{{-1, -1}};
		std::size_t          child_count = 0;
		for (; child_count < children.size(); ++child_count) {
			children[child_count] = fork();
			if (children[child_count] < 0) {
				ClosePipe(ready_pipe);
				ClosePipe(release_pipe);
				return ReapChildren(children, child_count, true);
			}
			if (children[child_count] == 0) {
				RunConcurrentWriterChild(base, ready_pipe, release_pipe, child_count);
			}
		}

		close(ready_pipe[1]);
		ready_pipe[1] = -1;
		close(release_pipe[0]);
		release_pipe[0]  = -1;
		char       ready = 0;
		const bool barriers_ready =
		    ReadPipeByte(ready_pipe[0], &ready) && ReadPipeByte(ready_pipe[0], &ready);
		close(ready_pipe[0]);
		ready_pipe[0]       = -1;
		const bool released = barriers_ready && WritePipeByte(release_pipe[1], 'g') &&
		                      WritePipeByte(release_pipe[1], 'g');
		close(release_pipe[1]);
		release_pipe[1] = -1;
		const bool children_succeeded =
		    ReapChildren(children, children.size(), !barriers_ready || !released);
		return barriers_ready && released && children_succeeded;
	}

	auto ConcurrentUniquePathInstall() -> bool {
		bool            ok        = true;
		const auto      temp_root = MakeTempRoot("concurrent-unique-path", ok);
		const auto      base      = temp_root / "log" / "snapshots" / "20260816T100012.jpg";
		std::error_code ec;
		ok &= Expect(fs::create_directories(base.parent_path(), ec) && !ec,
		             "concurrent install creates parent");
		ok &= Expect(WriteFile(base, "existing snapshot"), "concurrent install creates base");
		ok &=
		    Expect(chmod(base.c_str(), kSnapshotFileMode) == 0, "concurrent install secures base");
		if (!ok) {
			return CleanupTempRoot(temp_root, false);
		}
		ok &= Expect(RunConcurrentWriters(base), "concurrent install writers succeed");

		const auto first           = CandidatePathForTest(base, 1);
		const auto second          = CandidatePathForTest(base, 2);
		const auto first_contents  = ReadFile(first);
		const auto second_contents = ReadFile(second);
		ok &= Expect(ReadFile(base) == "existing snapshot",
		             "concurrent install preserves existing base");
		ok &= Expect((first_contents == "AAA" && second_contents == "BBB") ||
		                 (first_contents == "BBB" && second_contents == "AAA"),
		             "concurrent install preserves separate writer contents");
		ok &= Expect(CountStagedFiles(base.parent_path()) == 0,
		             "concurrent install leaves no staged files");
		return CleanupTempRoot(temp_root, ok);
	}

	auto DirectoryTargetRejectsBeforeEncoding() -> bool {
		bool            ok        = true;
		const auto      temp_root = MakeTempRoot("directory-target", ok);
		const auto      output    = temp_root / "log" / "snapshots" / "test.jpg";
		std::error_code ec;
		fs::create_directories(output, ec);
		ok &= Expect(!ec, "directory target created");
		WriterCallbackContext context;
		const bool            result = snapshot_internal::WriteSnapshotAtPath(
		    TinyFrames(), TextLines(), output, FakeDependencies(context, temp_root));
		ok &= Expect(!result, "directory target returns false");
		ok &= Expect(context.encode_calls == 0, "directory target skips encoder");
		ok &= Expect(fs::is_directory(output), "directory target remains directory");
		ok &= Expect(CountStagedFiles(output.parent_path()) == 0,
		             "directory target creates no stage");
		return CleanupTempRoot(temp_root, ok);
	}

	auto BlockedParentRejectsBeforeEncoding() -> bool {
		bool       ok        = true;
		const auto temp_root = MakeTempRoot("blocked-parent", ok);
		const auto blocker   = temp_root / "log";
		const auto output    = blocker / "snapshots" / "test.jpg";
		ok &= Expect(WriteFile(blocker, "blocking content"), "blocked parent creates blocker");
		WriterCallbackContext context;
		std::ostringstream    error;
		{
			StreamRedirect redirect(std::cerr, error.rdbuf());
			ok &=
			    Expect(!snapshot_internal::WriteSnapshotAtPath(
			               TinyFrames(), TextLines(), output, FakeDependencies(context, temp_root)),
			           "blocked parent returns false");
		}
		ok &= Expect(context.encode_calls == 0, "blocked parent skips encoder");
		ok &= Expect(ReadFile(blocker) == "blocking content", "blocked parent preserves blocker");
		return CleanupTempRoot(temp_root, ok);
	}

	struct ImageOutputCase {
		std::string name;
		std::string filename;
		std::string extension;
	};

	auto RealImageOutput(const ImageOutputCase &test_case) -> bool {
		bool                  ok        = true;
		const auto            temp_root = MakeTempRoot(test_case.name, ok);
		const auto            output    = temp_root / "log" / "snapshots" / test_case.filename;
		WriterCallbackContext context;
		const bool            result = snapshot_internal::WriteSnapshotAtPath(
		    TinyFrames(), TextLines(), output, RealDependencies(context, temp_root));
		ok &= Expect(result, test_case.name + " returns true");
		ok &=
		    Expect(!cv::imread(output.string()).empty(), test_case.name + " decodes successfully");
		ok &= Expect(context.received_extension == test_case.extension,
		             test_case.name + " receives exact extension");
		ok &= ExpectMode(output, kSnapshotFileMode, test_case.name + " output mode is 0600");
		return CleanupTempRoot(temp_root, ok);
	}

	auto ExtensionlessRejectsBeforeDirectorySetup() -> bool {
		bool                  ok        = true;
		const auto            temp_root = MakeTempRoot("extensionless", ok);
		const auto            log_root  = temp_root / "log";
		const auto            output    = log_root / "snapshots" / "snapshot";
		WriterCallbackContext context;
		const bool            result = snapshot_internal::WriteSnapshotAtPath(
		    TinyFrames(), TextLines(), output, FakeDependencies(context, temp_root));
		ok &= Expect(!result, "extensionless output returns false");
		ok &= Expect(context.encode_calls == 0, "extensionless output skips encoder");
		ok &= Expect(!fs::exists(log_root),
		             "extensionless output intentionally skips directory setup");
		ok &= Expect(!fs::exists(output), "extensionless output creates no destination");
		ok &= Expect(CountStagedFiles(output.parent_path()) == 0,
		             "extensionless output creates no staged file");
		return CleanupTempRoot(temp_root, ok);
	}

	auto InsecureExistingLogRootRejectsBeforeEncoding() -> bool {
		bool            ok        = true;
		const auto      temp_root = MakeTempRoot("insecure-log-root", ok);
		const auto      log_root  = temp_root / "log";
		const auto      output    = log_root / "snapshots" / "test.jpg";
		std::error_code ec;
		fs::create_directories(log_root, ec);
		ok &= Expect(!ec, "insecure log root created");
		ok &= Expect(chmod(log_root.c_str(), S_IRWXU | S_IRWXG) == 0,
		             "insecure log root made group-writable");
		WriterCallbackContext context;
		std::ostringstream    error;
		{
			StreamRedirect redirect(std::cerr, error.rdbuf());
			ok &=
			    Expect(!snapshot_internal::WriteSnapshotAtPath(
			               TinyFrames(), TextLines(), output, FakeDependencies(context, temp_root)),
			           "insecure log root returns false");
		}
		ok &= Expect(context.encode_calls == 0, "insecure log root skips encoder");
		ok &= Expect(!fs::exists(output), "insecure log root creates no output");
		ok &= Expect(error.str().contains("Log directory must not be group-writable"),
		             "insecure log root reports security diagnostic");
		return CleanupTempRoot(temp_root, ok);
	}

	auto MissingDirectoriesAreCreatedAndSecured() -> bool {
		bool                  ok            = true;
		const auto            temp_root     = MakeTempRoot("directory-creation", ok);
		const auto            log_root      = temp_root / "log";
		const auto            snapshots_dir = log_root / "snapshots";
		const auto            output        = snapshots_dir / "test.jpg";
		WriterCallbackContext context;
		const bool            result = snapshot_internal::WriteSnapshotAtPath(
		    TinyFrames(), TextLines(), output, FakeDependencies(context, temp_root));
		ok &= Expect(result, "directory creation returns true");
		ok &= Expect(context.encode_calls == 1, "directory creation calls encoder once");
		ok &= ExpectMode(log_root, kSnapshotDirectoryMode, "created log root mode is 0750");
		ok &= ExpectMode(snapshots_dir, kSnapshotDirectoryMode,
		                 "created snapshot directory mode is 0750");
		ok &= Expect(fs::exists(output), "directory creation creates output");
		return CleanupTempRoot(temp_root, ok);
	}

	auto MixedWidthFramesAreSupported() -> bool {
		bool                  ok        = true;
		const auto            temp_root = MakeTempRoot("mixed-width", ok);
		const auto            output    = temp_root / "log" / "snapshots" / "test.jpg";
		WriterCallbackContext context;
		const bool            result = snapshot_internal::WriteSnapshotAtPath(
		    {cv::Mat(8, 4, CV_8UC3, cv::Scalar(10, 20, 30)),
		     cv::Mat(8, 12, CV_8UC3, cv::Scalar(40, 50, 60))},
		    TextLines(), output, FakeDependencies(context, temp_root));
		ok &= Expect(result, "mixed-width frames return true");
		ok &= Expect(context.encode_calls == 1, "mixed-width frames call encoder once");
		return CleanupTempRoot(temp_root, ok);
	}

	auto CommittedSyncFailureIsReported() -> bool {
		bool                  ok        = true;
		const auto            temp_root = MakeTempRoot("committed-sync-failure", ok);
		const auto            output    = temp_root / "log" / "snapshots" / "test.jpg";
		WriterCallbackContext context;
		auto                  dependencies = FakeDependencies(context, temp_root);
		dependencies.sync_parent           = FailParentSync;
		howdy::native::AtomicFileCommitResult commit_result;
		const bool                            result = snapshot_internal::WriteSnapshotAtPath(
		    TinyFrames(), TextLines(), output, dependencies, &commit_result);
		ok &= Expect(!result, "committed sync failure returns false");
		ok &= Expect(commit_result == howdy::native::AtomicFileCommitResult::kCommittedSyncFailed,
		             "committed sync failure preserves exact commit state");
		ok &= Expect(fs::exists(output), "committed sync failure leaves snapshot visible");
		return CleanupTempRoot(temp_root, ok);
	}

}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= FrameValidationRegressions();
	ok &= MissingEncoderDependency();
	ok &= EncoderFailurePreservesDestination("encoder-false", false, false);
	ok &= EncoderFailurePreservesDestination("encoder-std-exception", true, false);
	ok &= EncoderFailurePreservesDestination("encoder-cv-exception", false, true);
	ok &= EmptyEncoderOutputFailsClosed();
	ok &= EncodedBytesInstall();
	ok &= ExistingDestinationIsNotReplaced();
	ok &= UniquePathInstallSelectsNextCandidate();
	ok &= UniquePathCollisionExhaustion();
	ok &= ConcurrentUniquePathInstall();
	ok &= DirectoryTargetRejectsBeforeEncoding();
	ok &= BlockedParentRejectsBeforeEncoding();
	ok &= RealImageOutput({.name = "jpeg-output", .filename = "test.jpg", .extension = ".jpg"});
	ok &= RealImageOutput({.name = "png-output", .filename = "test.png", .extension = ".png"});
	ok &= RealImageOutput({.name = "hidden-png-output", .filename = ".png", .extension = ".png"});
	ok &= ExtensionlessRejectsBeforeDirectorySetup();
	ok &= InsecureExistingLogRootRejectsBeforeEncoding();
	ok &= MissingDirectoriesAreCreatedAndSecured();
	ok &= MixedWidthFramesAreSupported();
	ok &= CommittedSyncFailureIsReported();
	return ok ? 0 : 1;
}
