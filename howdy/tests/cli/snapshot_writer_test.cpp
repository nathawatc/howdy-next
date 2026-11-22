#include "cli/snapshot/internal.hpp"
#include "cli/snapshot_writer_test_support.hpp"
#include "support/atomic_files.hpp"
#include "support/frame_limits.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <opencv2/core/hal/interface.h>
#include <opencv2/core/mat.hpp>
#include <opencv2/core/types.hpp>
#include <opencv2/imgcodecs.hpp>

#include <sys/stat.h>

namespace {

	using howdy::test::Expect;
	using howdy::test::ReadFile;
	using howdy::test::WriteFile;
	using howdy::test::snapshot_writer::CleanupTempRoot;
	using howdy::test::snapshot_writer::CountStagedFiles;
	using howdy::test::snapshot_writer::ExpectMode;
	using howdy::test::snapshot_writer::FakeDependencies;
	using howdy::test::snapshot_writer::kSnapshotFileMode;
	using howdy::test::snapshot_writer::MakeTempRoot;
	using howdy::test::snapshot_writer::TextLines;
	using howdy::test::snapshot_writer::TinyFrames;
	using howdy::test::snapshot_writer::WriterCallbackContext;

	namespace fs                = std::filesystem;
	namespace snapshot_internal = howdy::native::snapshot_internal;

	auto RealEncode(void *raw_context, std::string_view extension, const cv::Mat &image,
	                std::vector<uchar> *encoded) -> bool {
		auto *context = static_cast<WriterCallbackContext *>(raw_context);
		++context->encode_calls;
		context->received_extension = extension;
		return cv::imencode(std::string(extension), image, *encoded);
	}

	auto RealDependencies(WriterCallbackContext &context, const fs::path &root)
	    -> snapshot_internal::SnapshotWriterDependencies {
		return {.context = &context, .encode_image = RealEncode, .validation_root = {root}};
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
		bool ok      = true;
		auto frames  = TinyFrames();
		frames.at(1) = cv::Mat{};
		ok &= ExpectInvalidBatchRejected("empty-member", frames);
		ok &= ExpectInvalidBatchRejected("wrong-channel", {cv::Mat(8, 8, CV_8UC1, cv::Scalar(10))});
		ok &= ExpectInvalidBatchRejected("wrong-pixel-depth",
		                                 {cv::Mat(8, 8, CV_16UC3, cv::Scalar(10, 20, 30))});
		ok &= ExpectInvalidBatchRejected("combined-width",
		                                 {
		                                     cv::Mat(1, 4097, CV_8UC3, cv::Scalar(10, 20, 30)),
		                                     cv::Mat(1, 4097, CV_8UC3, cv::Scalar(40, 50, 60)),
		                                 });
		ok &= ExpectInvalidBatchRejected(
		    "oversized-frame",
		    {cv::Mat(howdy::native::kMaxFrameDimension + 1, 1, CV_8UC3, cv::Scalar(10, 20, 30))});
		ok &= ExpectInvalidBatchRejected("shape-mismatch",
		                                 {
		                                     cv::Mat(8, 8, CV_8UC3, cv::Scalar(10, 20, 30)),
		                                     cv::Mat(9, 8, CV_8UC3, cv::Scalar(40, 50, 60)),
		                                 });
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
		howdy::native::AtomicFileCommitResult commit_result =
		    howdy::native::AtomicFileCommitResult::kNotCommitted;
		const bool result = snapshot_internal::WriteSnapshotAtPath(
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

	auto MixedWidthFramesAreSupported() -> bool {
		bool                  ok        = true;
		const auto            temp_root = MakeTempRoot("mixed-width", ok);
		const auto            output    = temp_root / "log" / "snapshots" / "test.jpg";
		WriterCallbackContext context;
		const bool            result = snapshot_internal::WriteSnapshotAtPath(
		    {
		        cv::Mat(8, 4, CV_8UC3, cv::Scalar(10, 20, 30)),
		        cv::Mat(8, 12, CV_8UC3, cv::Scalar(40, 50, 60)),
		    },
		    TextLines(), output, FakeDependencies(context, temp_root));
		ok &= Expect(result, "mixed-width frames return true");
		ok &= Expect(context.encode_calls == 1, "mixed-width frames call encoder once");
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
	ok &= RealImageOutput({.name = "jpeg-output", .filename = "test.jpg", .extension = ".jpg"});
	ok &= RealImageOutput({.name = "png-output", .filename = "test.png", .extension = ".png"});
	ok &= RealImageOutput({.name = "hidden-png-output", .filename = ".png", .extension = ".png"});
	ok &= MixedWidthFramesAreSupported();
	ok &= howdy::test::snapshot_writer::RunSnapshotWriterSecurityTests();
	return ok ? 0 : 1;
}
