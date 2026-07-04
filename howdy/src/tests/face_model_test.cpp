#include "core/face_model.hpp"
#include "tests/include/core/face_model_test_access.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <utility>

namespace {
	constexpr auto kYunetSecretPath = "/secret/models/yunet.onnx";
	constexpr auto kSfaceSecretPath = "/secret/models/sface.onnx";
	constexpr auto kRawError        = "raw backend failure";
	constexpr auto kOpenCvSource    = "/opencv/modules/objdetect/src/face_detect.cpp";
	constexpr auto kOpenCvFunction  = "FaceDetectorYNImpl::detect";

	auto expect(bool condition, const std::string &message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << "\n";
			return false;
		}
		return true;
	}

	[[noreturn]] void throw_cv_error() {
		throw cv::Exception(cv::Error::StsError, kRawError, kOpenCvFunction, kOpenCvSource, 73);
	}

	class FakeDetector final : public cv::FaceDetectorYN {
	public:
		void setInputSize(const cv::Size &input_size) override {
			input_size_ = input_size;
		}

		auto getInputSize() -> cv::Size override {
			return input_size_;
		}

		void setScoreThreshold(float score_threshold) override {
			score_threshold_ = score_threshold;
		}

		auto getScoreThreshold() -> float override {
			return score_threshold_;
		}

		void setNMSThreshold(float nms_threshold) override {
			nms_threshold_ = nms_threshold;
		}

		auto getNMSThreshold() -> float override {
			return nms_threshold_;
		}

		void setTopK(int top_k) override {
			top_k_ = top_k;
		}

		auto getTopK() -> int override {
			return top_k_;
		}

		auto detect([[maybe_unused]] cv::InputArray image, cv::OutputArray faces) -> int override {
			faces.release();
			return 0;
		}

	private:
		cv::Size input_size_{320, 320};
		float    score_threshold_ = 0.9F;
		float    nms_threshold_   = 0.3F;
		int      top_k_           = 5000;
	};

	class FakeRecognizer final : public cv::FaceRecognizerSF {
	public:
		void alignCrop([[maybe_unused]] cv::InputArray src_img,
		               [[maybe_unused]] cv::InputArray face_box,
		               cv::OutputArray                 aligned) const override {
			aligned.release();
		}

		void feature([[maybe_unused]] cv::InputArray aligned_img,
		             cv::OutputArray                 feature) override {
			feature.release();
		}

		[[nodiscard]] auto match([[maybe_unused]] cv::InputArray face_feature1,
		                         [[maybe_unused]] cv::InputArray face_feature2,
		                         [[maybe_unused]] int dis_type) const -> double override {
			return 0.0;
		}
	};

	auto config() -> howdy::native::FaceConfig {
		auto value        = howdy::native::FaceConfig{};
		value.yunet_model = kYunetSecretPath;
		value.sface_model = kSfaceSecretPath;
		return value;
	}

	auto successful_backend() -> howdy::native::FaceModelTestAccess::Backend {
		return {
		    .check_readiness =
		        [](const std::filesystem::path &) {
			        return howdy::native::OpenCvModelReadiness{
			            .status = howdy::native::OpenCvModelStatus::kOk,
			        };
		        },
		    .create_detector =
		        [](const std::string &, const cv::Size &, float, float, int) {
			        return cv::makePtr<FakeDetector>();
		        },
		    .create_recognizer =
		        [](const std::string &) {
			        return cv::makePtr<FakeRecognizer>();
		        },
		    .set_input_size = [](const cv::Size &) {},
		    .detect =
		        [](const cv::Mat &, cv::Mat &faces) {
			        faces = cv::Mat{};
		        },
		};
	}

	auto sanitized(const std::string &message) -> bool {
		return !message.empty() && !message.contains(kYunetSecretPath) &&
		       !message.contains(kSfaceSecretPath) && !message.contains(kRawError) &&
		       !message.contains(kOpenCvSource) && !message.contains(kOpenCvFunction);
	}

	auto expect_init_failure(const howdy::native::FaceModel       &model,
	                         howdy::native::FaceModelErrorCategory expected_category,
	                         const std::string                    &expected_message) -> bool {
		bool ok = true;
		ok &= expect(!model.ok(), "model initialization fails");
		ok &= expect(model.error_category() == expected_category,
		             "initialization error category is retained");
		ok &= expect(model.error_message() == expected_message,
		             "initialization diagnostic is stable");
		ok &= expect(sanitized(model.error_message()), "initialization diagnostic is sanitized");
		return ok;
	}

	auto expect_detection_failure(howdy::native::FaceModel &model) -> bool {
		const auto result = model.detect(cv::Mat(480, 640, CV_8UC3, cv::Scalar(1, 2, 3)));
		bool       ok     = true;
		ok &= expect(result.status == howdy::native::FaceDetectionStatus::kInferenceError,
		             "OpenCV exception maps to inference error");
		ok &= expect(result.error_message == "Face detection failed",
		             "detection diagnostic is stable");
		ok &= expect(sanitized(result.error_message), "detection diagnostic is sanitized");
		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	{
		auto backend            = successful_backend();
		backend.create_detector = [](const std::string &, const cv::Size &, float, float,
		                             int) -> cv::Ptr<cv::FaceDetectorYN> {
			throw_cv_error();
		};
		auto model = howdy::native::FaceModelTestAccess::create(config(), std::move(backend));
		ok &= expect_init_failure(model,
		                          howdy::native::FaceModelErrorCategory::kDetectorInitialization,
		                          "Failed to initialize face detector");
	}

	{
		auto backend              = successful_backend();
		backend.create_recognizer = [](const std::string &) -> cv::Ptr<cv::FaceRecognizerSF> {
			throw_cv_error();
		};
		auto model = howdy::native::FaceModelTestAccess::create(config(), std::move(backend));
		ok &= expect_init_failure(model,
		                          howdy::native::FaceModelErrorCategory::kRecognizerInitialization,
		                          "Failed to initialize face recognizer");
	}

	{
		auto backend            = successful_backend();
		backend.create_detector = [](const std::string &, const cv::Size &, float, float, int) {
			return cv::Ptr<cv::FaceDetectorYN>{};
		};
		auto model = howdy::native::FaceModelTestAccess::create(config(), std::move(backend));
		ok &= expect_init_failure(model,
		                          howdy::native::FaceModelErrorCategory::kDetectorInitialization,
		                          "Failed to initialize face detector");
	}

	{
		auto backend              = successful_backend();
		backend.create_recognizer = [](const std::string &) {
			return cv::Ptr<cv::FaceRecognizerSF>{};
		};
		auto model = howdy::native::FaceModelTestAccess::create(config(), std::move(backend));
		ok &= expect_init_failure(model,
		                          howdy::native::FaceModelErrorCategory::kRecognizerInitialization,
		                          "Failed to initialize face recognizer");
	}

	{
		auto backend            = successful_backend();
		backend.check_readiness = [](const std::filesystem::path &) {
			return howdy::native::OpenCvModelReadiness{
			    .status        = howdy::native::OpenCvModelStatus::kInvalid,
			    .error_message = std::string(kYunetSecretPath) + ": " + kRawError,
			};
		};
		auto model = howdy::native::FaceModelTestAccess::create(config(), std::move(backend));
		ok &= expect_init_failure(model, howdy::native::FaceModelErrorCategory::kModelNotReady,
		                          "Face model is not ready");
	}

	{
		auto backend            = successful_backend();
		backend.check_readiness = [](const std::filesystem::path &path) {
			if (path == kSfaceSecretPath) {
				return howdy::native::OpenCvModelReadiness{
				    .status        = howdy::native::OpenCvModelStatus::kInvalid,
				    .error_message = std::string(kSfaceSecretPath) + ": " + kRawError,
				};
			}
			return howdy::native::OpenCvModelReadiness{
			    .status = howdy::native::OpenCvModelStatus::kOk,
			};
		};
		auto model = howdy::native::FaceModelTestAccess::create(config(), std::move(backend));
		ok &= expect_init_failure(model, howdy::native::FaceModelErrorCategory::kModelNotReady,
		                          "Face model is not ready");
	}

	{
		auto backend           = successful_backend();
		backend.set_input_size = [](const cv::Size &) {
			throw_cv_error();
		};
		auto model = howdy::native::FaceModelTestAccess::create(config(), std::move(backend));
		ok &= expect(model.ok(), "setInputSize failure model initializes");
		ok &= expect_detection_failure(model);
	}

	{
		auto backend   = successful_backend();
		backend.detect = [](const cv::Mat &, cv::Mat &) {
			throw_cv_error();
		};
		auto model = howdy::native::FaceModelTestAccess::create(config(), std::move(backend));
		ok &= expect(model.ok(), "detector failure model initializes");
		ok &= expect_detection_failure(model);
	}

	{
		bool input_size_set    = false;
		bool detect_called     = false;
		auto backend           = successful_backend();
		backend.set_input_size = [&input_size_set](const cv::Size &size) {
			input_size_set = size == cv::Size(640, 480);
		};
		backend.detect = [&detect_called](const cv::Mat &, cv::Mat &faces) {
			detect_called = true;
			faces         = cv::Mat(1, 15, CV_32FC1);
			for (int column = 0; column < faces.cols; ++column) {
				faces.at<float>(0, column) = static_cast<float>(column + 1);
			}
		};
		auto       model = howdy::native::FaceModelTestAccess::create(config(), std::move(backend));
		const auto result = model.detect(cv::Mat(480, 640, CV_8UC3, cv::Scalar(1, 2, 3)));
		ok &= expect(model.ok(), "success backend initializes");
		ok &= expect(model.error_category() == howdy::native::FaceModelErrorCategory::kNone,
		             "successful initialization has no error category");
		ok &= expect(input_size_set, "success path updates detector input size");
		ok &= expect(detect_called, "success path invokes detector");
		ok &= expect(result.ok() && result.detections.size() == 1,
		             "success path preserves parsed detection");
		if (result.detections.size() == 1) {
			ok &= expect(result.detections.front().box == cv::Rect2f(1, 2, 3, 4),
			             "success path preserves detection fields");
		}
	}

	return ok ? 0 : 1;
}
