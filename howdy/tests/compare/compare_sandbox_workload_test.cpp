#include "compare/sandbox.hpp"
#include "config/runtime_config.hpp"
#include "model_assets/opencv_model_manifest.hpp"
#include "support/cpu_topology.hpp"
#include "test_support.hpp"
#include "vision/face_model.hpp"
#include "vision/frame_processing.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>

#include <opencv2/core.hpp>
#include <opencv2/core/cvstd_wrapper.hpp>
#include <opencv2/core/exception.hpp>
#include <opencv2/core/hal/interface.h>
#include <opencv2/core/mat.hpp>
#include <opencv2/core/types.hpp>
#include <opencv2/core/utility.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect/face.hpp>

#include <sys/types.h>
#include <sys/wait.h>

// Runs the production seccomp filter over the OpenCV work the compare frame loop
// performs, in a child process, against whatever OpenCV and C library the host
// provides. The hand-written allowlist is validated against glibc and OpenCV
// builds that differ per distribution (threading backend, BLAS, malloc paths):
// a missing syscall kills the child with SIGSYS and fails this test.
//
// Modes:
//   synthetic-warm  OpenCV's thread pool is created before the filter, as
//                   production does when it warms the model.
//   synthetic-cold  The thread pool is first created under the filter.
//   models          Real YuNet/SFace inference; needs HOWDY_TEST_MODELS_DIR.

namespace {

	namespace fs = std::filesystem;

	using howdy::test::Expect;

	constexpr std::string_view kProbeFlag              = "--workload-probe";
	constexpr int              kExitFailure            = 1;
	constexpr int              kExitSandbox            = 2;
	constexpr int              kExitModelLoad          = 3;
	constexpr int              kExitSeccompFailure     = 4;
	constexpr int              kExitException          = 5;
	constexpr int              kExitSeccompUnsupported = 77;
	constexpr int              kSandboxTimeoutSeconds  = 5;
	constexpr int              kFrameWidth             = 640;
	constexpr int              kFrameHeight            = 480;
	constexpr int              kFramesPerRun           = 12;

	void Phase(std::string_view name) {
		std::cerr << "phase: " << name << '\n';
	}

	auto MakeSyntheticFrame(int seed) -> cv::Mat {
		cv::Mat frame(kFrameHeight, kFrameWidth, CV_8UC1);
		cv::RNG rng(static_cast<std::uint64_t>(seed) + 1U);
		rng.fill(frame, cv::RNG::UNIFORM, 0, 256);
		return frame;
	}

	void ExerciseParallelBackend() {
		std::vector<double> sums(64, 0.0);
		cv::parallel_for_(cv::Range(0, static_cast<int>(sums.size())),
		                  [&sums](const cv::Range &range) -> void {
			                  for (int i = range.start; i < range.end; ++i) {
				                  double total = 0.0;
				                  for (int j = 0; j < 20000; ++j) {
					                  total += static_cast<double>(i * j % 7);
				                  }
				                  sums.at(static_cast<std::size_t>(i)) = total;
			                  }
		                  });
	}

	struct Models {
		cv::Ptr<cv::FaceDetectorYN>   detector;
		cv::Ptr<cv::FaceRecognizerSF> recognizer;
	};

	auto LoadModels(const fs::path &directory) -> std::optional<Models> {
		Models models;
		try {
			models.detector = cv::FaceDetectorYN::create(
			    (directory / howdy::native::kYunetModelDescriptor.filename).string(), "",
			    cv::Size(320, 320), 0.6F, 0.3F, 5000);
			models.recognizer = cv::FaceRecognizerSF::create(
			    (directory / howdy::native::kSfaceModelDescriptor.filename).string(), "");
		} catch (const cv::Exception &error) {
			std::cerr << "FAIL: model load: " << error.what() << '\n';
			return std::nullopt;
		}
		if (models.detector.empty() || models.recognizer.empty()) {
			return std::nullopt;
		}
		return models;
	}

	// A detector row: x, y, w, h, five landmarks (x, y), score. Lets the recognizer
	// run without depending on the detector finding a face in noise.
	auto MakeFaceRow() -> cv::Mat {
		const std::array<float, 15> values = {
		    100, 100, 200, 200, 160, 160, 240, 160, 200, 200, 165, 250, 235, 250, 0.99F,
		};
		cv::Mat row(1, 15, CV_32F);
		for (int i = 0; i < 15; ++i) {
			row.at<float>(0, i) = values.at(static_cast<std::size_t>(i));
		}
		return row;
	}

	void RunInference(Models &models, const cv::Mat &gray) {
		const cv::Mat bgr = howdy::native::FaceModel::PrepareFrame(gray);
		models.detector->setInputSize(bgr.size());
		cv::Mat faces;
		models.detector->detect(bgr, faces);

		const cv::Mat face = MakeFaceRow();
		cv::Mat       aligned;
		cv::Mat       feature_a;
		cv::Mat       feature_b;
		models.recognizer->alignCrop(bgr, face, aligned);
		models.recognizer->feature(aligned, feature_a);
		models.recognizer->feature(aligned, feature_b);
		(void)models.recognizer->match(feature_a, feature_b,
		                               cv::FaceRecognizerSF::DisType::FR_COSINE);
	}

	auto RunWorkloadChild(std::string_view mode, const fs::path &models_directory) -> int {
		const bool use_models = mode == "models";
		const bool warm       = mode == "synthetic-warm";

		// Same order as compare: environment, thread count, sandbox, model load and
		// warm-up, seccomp, then the frame loop.
		if (!howdy::native::ConfigureFaceModelEnvironment()) {
			return kExitFailure;
		}
		if (const auto cores = howdy::native::AvailablePhysicalCoreCount(); cores.has_value()) {
			cv::setNumThreads(*cores);
		}

		Phase("sandbox");
		const auto sandbox = howdy::native::ApplyCompareSandbox(kSandboxTimeoutSeconds);
		if (sandbox.status != howdy::native::CompareSandboxStatus::kOk) {
			std::cerr << "FAIL: sandbox limits not applied (status "
			          << static_cast<int>(sandbox.status) << ", errno " << sandbox.error_number
			          << ")\n";
			return kExitSandbox;
		}

		std::optional<Models> models;
		if (use_models) {
			Phase("model load");
			models = LoadModels(models_directory);
			if (!models.has_value()) {
				return kExitModelLoad;
			}
		}

		const howdy::native::VideoConfig video = [] -> howdy::native::VideoConfig {
			auto config                 = howdy::native::DefaultVideoConfig();
			config.clahe_enabled        = true;
			config.clahe_clip_limit     = 2.0F;
			config.clahe_tile_grid_size = 8;
			return config;
		}();
		auto clahe = howdy::native::MakeClahe(video);

		Phase("warm-up");
		if (models.has_value()) {
			RunInference(*models, MakeSyntheticFrame(0));
		} else if (warm) {
			ExerciseParallelBackend();
		}

		Phase("seccomp");
		const auto seccomp = howdy::native::ApplyCompareSeccompFilter();
		if (seccomp.status == howdy::native::CompareSeccompStatus::kUnsupported) {
			return kExitSeccompUnsupported;
		}
		if (seccomp.status != howdy::native::CompareSeccompStatus::kOk) {
			std::cerr << "FAIL: seccomp filter not installed (status "
			          << static_cast<int>(seccomp.status) << ", errno " << seccomp.error_number
			          << ")\n";
			return kExitSeccompFailure;
		}

		Phase("frame loop");
		for (int frame_number = 0; frame_number < kFramesPerRun; ++frame_number) {
			cv::Mat gray = MakeSyntheticFrame(frame_number);
			(void)howdy::native::MeasureBrightness(gray);
			howdy::native::ApplyClaheIfEnabled(gray, video, clahe);
			const cv::Mat rotated = howdy::native::RotateFrame(gray, video, frame_number);
			cv::Mat       resized;
			cv::resize(rotated, resized, cv::Size(), 0.5, 0.5, cv::INTER_AREA);
			(void)howdy::native::FaceModel::PrepareFrame(resized);
			ExerciseParallelBackend();
			if (models.has_value() && frame_number % 4 == 0) {
				RunInference(*models, gray);
			}
		}

		Phase("allocation churn");
		for (int i = 0; i < 16; ++i) {
			const cv::Mat large(2048, 2048, CV_8UC3, cv::Scalar::all(i));
			cv::Mat       blurred;
			cv::GaussianBlur(large, blurred, cv::Size(5, 5), 0);
		}

		Phase("teardown");
		models.reset();
		return 0;
	}

	auto SpawnWorkloadChild(std::string_view mode, const std::string &models_directory)
	    -> std::optional<int> {
		std::string           program("howdy_compare_sandbox_workload_test");
		std::string           flag(kProbeFlag);
		std::string           mode_argument(mode);
		std::string           models_argument(models_directory);
		std::array<char *, 5> arguments = {
		    program.data(), flag.data(), mode_argument.data(), models_argument.data(), nullptr,
		};
		// An empty environment keeps inherited LD_PRELOAD or sanitizer variables from
		// running extra syscalls before the filter is installed.
		std::array<char *, 1> environment = {nullptr};
		pid_t                 pid         = -1;
		if (posix_spawn(&pid, "/proc/self/exe", nullptr, nullptr, arguments.data(),
		                environment.data()) != 0) {
			return std::nullopt;
		}
		int status = 0;
		while (waitpid(pid, &status, 0) < 0) {
			if (errno != EINTR) {
				return std::nullopt;
			}
		}
		return status;
	}

	auto ExpectWorkloadSurvivesFilter(std::string_view mode, const std::string &models_directory)
	    -> bool {
		const auto status = SpawnWorkloadChild(mode, models_directory);
		if (!status.has_value()) {
			return Expect(false,
			              std::string("spawn and reap workload child: ") + std::string(mode));
		}
		if (WIFSIGNALED(*status)) {
			const int signal_number = WTERMSIG(*status);
			return Expect(
			    false, std::string(mode) + ": child killed by signal " +
			               std::to_string(signal_number) + " (" + strsignal(signal_number) + ")" +
			               (signal_number == SIGSYS
			                    ? "; the seccomp allowlist is missing a syscall used here (see the "
			                      "last 'phase:' line above)"
								: ""));
		}
		if (WIFEXITED(*status) && WEXITSTATUS(*status) == kExitSeccompUnsupported) {
			return howdy::test::SkipUnavailable("seccomp", "seccomp unsupported on this kernel");
		}
		return Expect(WIFEXITED(*status) && WEXITSTATUS(*status) == 0,
		              std::string(mode) + ": child exits cleanly under the filter (exit " +
		                  std::to_string(WIFEXITED(*status) ? WEXITSTATUS(*status) : -1) + ")");
	}

}  // namespace

auto main(int argc, char **argv) -> int {
	const auto arguments =
	    std::span(argv, argc > 0 ? static_cast<std::size_t>(argc) : std::size_t{0});
	if (arguments.size() == 4 && std::string_view(arguments.subspan(1).front()) == kProbeFlag) {
		// An uncaught exception would reach abort(), which the filter kills with SIGSYS and
		// hides the real cause (for example OpenCV out of memory under RLIMIT_AS).
		try {
			return RunWorkloadChild(arguments.subspan(2).front(), arguments.back());
		} catch (const std::exception &error) {
			std::cerr << "FAIL: uncaught exception in workload child: " << error.what() << '\n';
			return kExitException;
		}
	}

	bool ok = true;
	ok &= ExpectWorkloadSurvivesFilter("synthetic-warm", "");
	ok &= ExpectWorkloadSurvivesFilter("synthetic-cold", "");

	const char *const models_directory = std::getenv("HOWDY_TEST_MODELS_DIR");
	if (models_directory != nullptr && *models_directory != '\0') {
		ok &= ExpectWorkloadSurvivesFilter("models", models_directory);
	} else {
		ok &= howdy::test::SkipUnavailable(
		    "models", "HOWDY_TEST_MODELS_DIR is not set; real-model inference not run");
	}
	return ok ? 0 : 1;
}
