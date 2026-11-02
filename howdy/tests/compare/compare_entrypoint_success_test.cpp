#include "compare/capture_session.hpp"
#include "compare/entrypoint.hpp"
#include "compare/entrypoint/dependencies.hpp"
#include "compare/privileges.hpp"
#include "compare/sandbox.hpp"
#include "protocol/compare_exit.hpp"
#include "support/file_security/validation_root.hpp"
#include "test_support.hpp"
#include "vision/face_detection.hpp"
#include "vision/face_encoding.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <stdlib.h>
#include <unistd.h>

#include <opencv2/core/hal/interface.h>
#include <opencv2/core/mat.hpp>
#include <opencv2/core/types.hpp>
#include <opencv2/imgproc.hpp>

#include <sys/stat.h>
#include <sys/types.h>

#ifndef HOWDY_PACKAGED_CONFIG_PATH
#	error "HOWDY_PACKAGED_CONFIG_PATH must be defined by CMake"
#endif
#ifndef HOWDY_COMPARE_SUCCESS_ROOT
#	error "HOWDY_COMPARE_SUCCESS_ROOT must be defined by CMake"
#endif

namespace {
	namespace fs = std::filesystem;

	struct FixtureContext {
		int              open_calls    = 0;
		int              read_calls    = 0;
		int              prepare_calls = 0;
		int              detect_calls  = 0;
		int              encode_calls  = 0;
		int              drop_calls    = 0;
		int              seccomp_calls = 0;
		std::atomic<int> warm_calls{0};
		bool             encoded_after_warm = false;
		float            probe_value        = 1.0F;
	};

	auto OpenCapture(void *raw_context) -> bool {
		++static_cast<FixtureContext *>(raw_context)->open_calls;
		return true;
	}

	auto WarmUpCapture([[maybe_unused]] void *raw_context) -> bool {
		return true;
	}

	auto SetProperty([[maybe_unused]] void                          *raw_context,
	                 [[maybe_unused]] howdy::native::CaptureProperty property) -> bool {
		return true;
	}

	auto ReadGrayFrame(void *raw_context, cv::Mat &frame) -> bool {
		auto &context = *static_cast<FixtureContext *>(raw_context);
		if (++context.read_calls != 1) {
			return false;
		}
		frame = cv::Mat(32, 32, CV_8UC1, cv::Scalar(180));
		return true;
	}

	// NOLINTNEXTLINE(modernize-use-string-view): must match the std::string-returning callback type
	auto CaptureError([[maybe_unused]] void *raw_context) -> std::string {
		return "Unexpected second capture read";
	}

	auto FixedNow([[maybe_unused]] void *raw_context) -> std::chrono::steady_clock::time_point {
		return {};
	}

	auto PrepareFrame(void *raw_context, const cv::Mat &frame) -> cv::Mat {
		++static_cast<FixtureContext *>(raw_context)->prepare_calls;
		cv::Mat color;
		cv::cvtColor(frame, color, cv::COLOR_GRAY2BGR);
		return color;
	}

	auto DetectFaces(void *raw_context, [[maybe_unused]] const cv::Mat &frame)
	    -> howdy::native::FaceDetectionResult {
		++static_cast<FixtureContext *>(raw_context)->detect_calls;
		return {
		    .status = howdy::native::FaceDetectionStatus::kOk,
		    .detections =
		        {
		            {
		                .box        = cv::Rect2f(2.0F, 2.0F, 10.0F, 10.0F),
		                .landmarks  = {},
		                .confidence = 0.95F,
		            },
		        },
		};
	}

	auto EncodeFace(void *raw_context, [[maybe_unused]] const cv::Mat &frame,
	                [[maybe_unused]] const howdy::native::FaceDetection &face)
	    -> howdy::native::FaceEncodingResult {
		auto &context = *static_cast<FixtureContext *>(raw_context);
		++context.encode_calls;
		context.encoded_after_warm = context.warm_calls.load() == 1;
		return {
		    .status   = howdy::native::FaceEncodingStatus::kOk,
		    .encoding = std::vector<float>(howdy::native::kSfaceEmbeddingSize, context.probe_value),
		    .error_message = {},
		};
	}

	auto DropPrivileges(void *raw_context) -> howdy::native::ComparePrivilegeResult {
		++static_cast<FixtureContext *>(raw_context)->drop_calls;
		return {.status = howdy::native::ComparePrivilegeStatus::kOk};
	}

	auto WarmModel(void *raw_context) -> bool {
		++static_cast<FixtureContext *>(raw_context)->warm_calls;
		return true;
	}

	auto ApplySeccomp(void *raw_context) -> howdy::native::CompareSeccompResult {
		++static_cast<FixtureContext *>(raw_context)->seccomp_calls;
		return {};
	}

	auto UnsupportedSeccomp(void *raw_context) -> howdy::native::CompareSeccompResult {
		++static_cast<FixtureContext *>(raw_context)->seccomp_calls;
		return {
		    .status       = howdy::native::CompareSeccompStatus::kUnsupported,
		    .error_number = ENOSYS,
		};
	}

	auto FailedSeccomp(void *raw_context) -> howdy::native::CompareSeccompResult {
		++static_cast<FixtureContext *>(raw_context)->seccomp_calls;
		return {
		    .status       = howdy::native::CompareSeccompStatus::kInstallationFailure,
		    .error_number = ENOMEM,
		};
	}

	auto DependenciesFor(FixtureContext &context) -> howdy::native::CompareEntrypointDependencies {
		return {
		    .capture =
		        {
		            .capture_context = &context,
		            .open_capture    = OpenCapture,
		            .warm_up_capture = WarmUpCapture,
		            .read_gray_frame = ReadGrayFrame,
		            .error_message   = CaptureError,
		            .set_property    = SetProperty,
		            .clock_context   = &context,
		            .now             = FixedNow,
		        },
		    .inference =
		        {
		            .context       = &context,
		            .prepare_frame = PrepareFrame,
		            .detect_faces  = DetectFaces,
		            .encode_face   = EncodeFace,
		        },
		    .context         = &context,
		    .drop_privileges = DropPrivileges,
		    .warm_model      = WarmModel,
		    .apply_seccomp   = ApplySeccomp,
		};
	}

	auto PrepareFixture(const fs::path &root) -> bool {
		std::error_code error;
		fs::remove_all(root, error);
		error.clear();
		if (!fs::create_directories(root / "models", error) || error) {
			return false;
		}
		fs::copy_file(HOWDY_PACKAGED_CONFIG_PATH, root / "config.ini", error);
		if (error) {
			return false;
		}
		std::string config = howdy::test::ReadFile(root / "config.ini");
		const auto  report = config.find("end_report = false");
		if (report == std::string::npos) {
			return false;
		}
		config.replace(report, sizeof("end_report = false") - 1, "end_report = true");
		if (!howdy::test::WriteFile(root / "config.ini", config)) {
			return false;
		}
		std::string model =
		    R"([{"id":7,"time":1,"label":"fixture","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[)";
		for (std::size_t index = 0; index < howdy::native::kSfaceEmbeddingSize; ++index) {
			if (index != 0) {
				model += ',';
			}
			model += "1.0";
		}
		model += "]] }]";
		const auto model_path = root / "models" / "fixture-user.dat";
		return howdy::test::WriteFile(model_path, model) && chmod(root.c_str(), 0755) == 0 &&
		       chmod((root / "models").c_str(), 0755) == 0 &&
		       chmod((root / "config.ini").c_str(), 0644) == 0 &&
		       chmod(model_path.c_str(), 0600) == 0;
	}

}  // namespace

auto main() -> int {
	using howdy::test::Expect;
	const fs::path root(HOWDY_COMPARE_SUCCESS_ROOT);
	if (!Expect(PrepareFixture(root), "prepare secure compare success fixture")) {
		return 1;
	}
	const auto models_path = (root / "models").string();
	bool       ok          = Expect(setenv("HOWDY_USER_MODELS_DIR", models_path.c_str(), 1) == 0,
	                                "select fixture user-model directory");
	std::array<std::string, 4> arguments{
	    "howdy-compare",
	    "--config",
	    (root / "config.ini").string(),
	    "fixture-user",
	};
	std::array<char *, 4> argv{
	    arguments.at(0).data(),
	    arguments.at(1).data(),
	    arguments.at(2).data(),
	    arguments.at(3).data(),
	};
	const auto validation_root =
	    howdy::native::file_security_internal::ValidationRoot{.path = root};

	FixtureContext           success;
	const auto               dependencies = DependenciesFor(success);
	const std::ostringstream report;
	auto                    *original = std::cout.rdbuf(report.rdbuf());
	const int                result   = howdy::native::RunCompareEntrypoint(
	    static_cast<int>(argv.size()), argv.data(), geteuid(), validation_root, dependencies);
	std::cout.rdbuf(original);
	ok &= Expect(result == static_cast<int>(howdy::native::CompareExit::kSuccess),
	             "production compare entrypoint returns kSuccess after matching fixture face");
	ok &=
	    Expect(success.open_calls == 1 && success.read_calls == 1 && success.prepare_calls == 1 &&
		           success.detect_calls == 1 && success.encode_calls == 1 &&
		           success.drop_calls == 1 && success.seccomp_calls == 1 &&
		           success.warm_calls.load() == 1 && success.encoded_after_warm,
		       "entrypoint runs capture, preprocessing, inference, sandbox, and privilege stages");
	ok &= Expect(report.str().contains("Winning model: 7 (\"fixture\")"),
	             "success report identifies the loaded stored model");

	FixtureContext rejection;
	rejection.probe_value                           = -1.0F;
	const auto               rejection_dependencies = DependenciesFor(rejection);
	const std::ostringstream rejection_error;
	original = std::cerr.rdbuf(rejection_error.rdbuf());
	const int rejection_result =
	    howdy::native::RunCompareEntrypoint(static_cast<int>(argv.size()), argv.data(), geteuid(),
		                                    validation_root, rejection_dependencies);
	std::cerr.rdbuf(original);
	ok &= Expect(rejection_result == static_cast<int>(howdy::native::CompareExit::kInvalidDevice) &&
	                 rejection.read_calls == 2 && rejection.encode_calls == 1 &&
	                 rejection.seccomp_calls == 1 &&
	                 rejection_error.str().contains("Unexpected second capture read"),
	             "nonmatching embedding continues capture and does not authenticate");

	FixtureContext unsupported;
	auto           unsupported_dependencies = DependenciesFor(unsupported);
	unsupported_dependencies.apply_seccomp  = UnsupportedSeccomp;
	const std::ostringstream unsupported_error;
	original = std::cerr.rdbuf(unsupported_error.rdbuf());
	const std::ostringstream unsupported_report;
	auto                    *original_out = std::cout.rdbuf(unsupported_report.rdbuf());
	const int                unsupported_result =
	    howdy::native::RunCompareEntrypoint(static_cast<int>(argv.size()), argv.data(), geteuid(),
		                                    validation_root, unsupported_dependencies);
	std::cout.rdbuf(original_out);
	std::cerr.rdbuf(original);
	ok &= Expect(
	    unsupported_result == static_cast<int>(howdy::native::CompareExit::kSuccess) &&
	        unsupported.seccomp_calls == 1 &&
	        unsupported_error.str().contains("Seccomp filter sandboxing is not supported"),
	    "best-effort seccomp logs warning and continues authentication on unsupported kernel");

	FixtureContext failed;
	auto           failed_dependencies = DependenciesFor(failed);
	failed_dependencies.apply_seccomp  = FailedSeccomp;
	const std::ostringstream failed_error;
	original = std::cerr.rdbuf(failed_error.rdbuf());
	const int failed_result =
	    howdy::native::RunCompareEntrypoint(static_cast<int>(argv.size()), argv.data(), geteuid(),
		                                    validation_root, failed_dependencies);
	std::cerr.rdbuf(original);
	ok &= Expect(failed_result == static_cast<int>(howdy::native::CompareExit::kAbort) &&
	                 failed.seccomp_calls == 1 && failed.read_calls == 0,
	             "seccomp installation failure aborts before reading any frame");

	// Each fail-closed case must abort before any capture read.
	const auto expect_abort = [&](const char *label, uid_t owner, auto configure,
	                              const char *diagnostic) -> bool {
		FixtureContext context;
		auto           case_dependencies = DependenciesFor(context);
		configure(case_dependencies);
		const std::ostringstream case_error;
		auto                    *saved  = std::cerr.rdbuf(case_error.rdbuf());
		const int                status = howdy::native::RunCompareEntrypoint(
		    static_cast<int>(argv.size()), argv.data(), owner, validation_root, case_dependencies);
		std::cerr.rdbuf(saved);
		return Expect(status == static_cast<int>(howdy::native::CompareExit::kAbort) &&
		                  context.read_calls == 0 && !case_error.str().empty() &&
		                  case_error.str().contains(diagnostic),
		              label);
	};
	const auto keep = [](howdy::native::CompareEntrypointDependencies &) -> void {};
	const auto fail_drop =
	    [](howdy::native::CompareEntrypointDependencies &case_dependencies) -> void {
		case_dependencies.drop_privileges = [](void *) -> howdy::native::ComparePrivilegeResult {
			return {
			    .status        = howdy::native::ComparePrivilegeStatus::kVerificationFailure,
			    .error_message = "injected",
			};
		};
	};
	const auto config_file = root / "config.ini";
	const auto model_file  = root / "models" / "fixture-user.dat";

	ok &= expect_abort("failing privilege drop aborts before reading any frame", geteuid(),
	                   fail_drop, "Failed to drop compare privileges: injected");
	ok &= expect_abort("untrusted config owner aborts", geteuid() + 1, keep,
	                   "Config directory must be owned by UID");
	ok &= Expect(chmod(config_file.c_str(), 0666) == 0, "make config world-writable");
	ok &= expect_abort("world-writable config aborts", geteuid(), keep,
	                   "Config file must not be group-writable");
	ok &= Expect(chmod(config_file.c_str(), 0644) == 0, "restore config mode");
	ok &= Expect(chmod(model_file.c_str(), 0666) == 0, "make user model world-writable");
	ok &= expect_abort("world-writable user model aborts", geteuid(), keep,
	                   "User model file must not be group-writable");
	ok &= Expect(chmod(model_file.c_str(), 0600) == 0, "restore user model mode");
	ok &= Expect(howdy::test::WriteFile(model_file, "not json") &&
	                 chmod(model_file.c_str(), 0600) == 0,
	             "write malformed user model");
	ok &= expect_abort("malformed user model aborts", geteuid(), keep,
	                   "Failed to parse user model JSON");

	std::error_code cleanup_error;
	fs::remove_all(root, cleanup_error);
	ok &= Expect(!cleanup_error, "clean compare success fixture");
	return ok ? 0 : 1;
}
