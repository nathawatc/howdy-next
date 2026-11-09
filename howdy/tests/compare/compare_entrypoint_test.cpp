#include "compare/entrypoint.hpp"
#include "compare/entrypoint/internal.hpp"
#include "config/runtime_config.hpp"
#include "model_assets/model_file.hpp"
#include "protocol/compare_exit.hpp"
#include "support/file_security/validation_root.hpp"
#include "test_support.hpp"
#include "vision/face_model_test_access.hpp"

#include <array>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include <stdlib.h>
#include <unistd.h>

#include <opencv2/core/cvstd_wrapper.hpp>
#include <opencv2/core/hal/interface.h>
#include <opencv2/core/mat.hpp>
#include <opencv2/core/types.hpp>
#include <opencv2/objdetect/face.hpp>

#ifndef HOWDY_PACKAGED_CONFIG_PATH
#	error "HOWDY_PACKAGED_CONFIG_PATH must be defined by CMake"
#endif
#ifndef HOWDY_COMPARE_NO_MODEL_ROOT
#	error "HOWDY_COMPARE_NO_MODEL_ROOT must be defined by CMake"
#endif

namespace {

	namespace fs = std::filesystem;

	struct WarmCounts {
		int alignments = 0;
		int features   = 0;
	};

	class FakeDetector final : public cv::FaceDetectorYN {
	public:
		void setInputSize([[maybe_unused]] const cv::Size &input_size) override {}

		auto getInputSize() -> cv::Size override {
			return {320, 320};
		}

		void setScoreThreshold([[maybe_unused]] float score_threshold) override {}

		auto getScoreThreshold() -> float override {
			return 0.9F;
		}

		void setNMSThreshold([[maybe_unused]] float nms_threshold) override {}

		auto getNMSThreshold() -> float override {
			return 0.3F;
		}

		void setTopK([[maybe_unused]] int top_k) override {}

		auto getTopK() -> int override {
			return 5000;
		}

		auto detect([[maybe_unused]] cv::InputArray image, cv::OutputArray faces) -> int override {
			faces.release();
			return 0;
		}
	};

	class FakeRecognizer final : public cv::FaceRecognizerSF {
	public:
		explicit FakeRecognizer(WarmCounts *counts)
		    : counts_(counts) {}

		// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
		void alignCrop(cv::InputArray image, [[maybe_unused]] cv::InputArray face,
		               cv::OutputArray aligned) const override {
			++counts_->alignments;
			image.getMat().copyTo(aligned);
		}

		void feature([[maybe_unused]] cv::InputArray aligned, cv::OutputArray feature) override {
			++counts_->features;
			cv::Mat(1, 128, CV_32FC1, cv::Scalar(1.0F)).copyTo(feature);
		}

		[[nodiscard]] auto match([[maybe_unused]] cv::InputArray first,
		                         [[maybe_unused]] cv::InputArray second,
		                         [[maybe_unused]] int dis_type) const -> double override {
			return 0.0;
		}

	private:
		WarmCounts *counts_;
	};

	auto TestProductionWarmBridge() -> bool {
		WarmCounts counts;
		int        yunet_calls = 0;
		auto       backend     = howdy::native::FaceModelTestAccess::Backend{
		    .check_readiness = [](const fs::path &) -> howdy::native::OpenCvModelReadiness {
			    return {.status = howdy::native::OpenCvModelStatus::kOk};
		    },
		    .create_detector = [](const std::string &, const cv::Size &, float, float,
			                      int) -> cv::Ptr<cv::FaceDetectorYN> {
			    return cv::makePtr<FakeDetector>();
		    },
		    .create_recognizer = [&counts](const std::string &) -> cv::Ptr<cv::FaceRecognizerSF> {
			    return cv::makePtr<FakeRecognizer>(&counts);
		    },
		    .set_input_size = [](const cv::Size &) -> void {},
		    .detect         = [&yunet_calls](const cv::Mat &, cv::Mat &faces) -> void {
			    ++yunet_calls;
			    faces.release();
		    },
		};
		auto model = howdy::native::FaceModelTestAccess::Create(howdy::native::DefaultFaceConfig(),
		                                                        std::move(backend));
		if (!howdy::test::Expect(model.Ok() && counts.alignments == 0 && counts.features == 0,
		                         "production model begins without recognizer warm-up")) {
			return false;
		}
		return howdy::test::Expect(
		    howdy::native::compare_entrypoint_internal::WarmModel(model) &&
		        counts.alignments == 1 && counts.features == 1 && yunet_calls == 0,
		    "production warm bridge invokes SFace alignment and feature without YuNet");
	}

	auto PrepareFixture(const fs::path &root) -> bool {
		std::error_code error;
		fs::remove_all(root, error);
		error.clear();
		if (!fs::create_directories(root / "models", error) || error) {
			return false;
		}
		fs::copy_file(HOWDY_PACKAGED_CONFIG_PATH, root / "config.toml",
		              fs::copy_options::overwrite_existing, error);
		if (error) {
			return false;
		}
		fs::permissions(root,
		                fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
		                    fs::perms::others_read | fs::perms::others_exec,
		                fs::perm_options::replace, error);
		if (error) {
			return false;
		}
		fs::permissions(root / "models",
		                fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
		                    fs::perms::others_read | fs::perms::others_exec,
		                fs::perm_options::replace, error);
		if (error) {
			return false;
		}
		fs::permissions(root / "config.toml",
		                fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
		                    fs::perms::others_read,
		                fs::perm_options::replace, error);
		return !error;
	}

}  // namespace

auto main() -> int {
	using howdy::test::Expect;

	const fs::path root(HOWDY_COMPARE_NO_MODEL_ROOT);
	if (!Expect(PrepareFixture(root), "prepare secure compare no-model fixture")) {
		return 1;
	}

	const auto config_path = (root / "config.toml").string();
	const auto models_path = (root / "models").string();
	bool       ok          = true;
	ok &= TestProductionWarmBridge();
	ok &= Expect(setenv("HOWDY_CONFIG", config_path.c_str(), 1) == 0, "set compare config path");
	ok &= Expect(setenv("HOWDY_USER_MODELS_DIR", models_path.c_str(), 1) == 0,
	             "set empty user-model directory");

	std::array<std::string, 2> arguments{"howdy-compare", "coverage-user"};
	std::array<char *, 2>      argv{arguments.at(0).data(), arguments.at(1).data()};
	const int                  result = howdy::native::RunCompareEntrypoint(
	    static_cast<int>(argv.size()), argv.data(), geteuid(),
	    howdy::native::file_security_internal::ValidationRoot{.path = root});
	ok &= Expect(result == static_cast<int>(howdy::native::CompareExit::kNoFaceModel),
	             "empty secure user-model directory returns kNoFaceModel");

	std::error_code cleanup_error;
	fs::remove_all(root, cleanup_error);
	ok &= Expect(!cleanup_error, "clean compare no-model fixture");
	return ok ? 0 : 1;
}
