#include "config/runtime_paths.hpp"
#include "core/face_model.hpp"
#include "tests/include/core/face_model_test_access.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

namespace {
	constexpr auto kYunetSecretPath = "/secret/models/yunet.onnx";
	constexpr auto kSfaceSecretPath = "/secret/models/sface.onnx";
	constexpr auto kRawError        = "raw backend failure";
	constexpr auto kOpenCvSource    = "/opencv/modules/objdetect/src/face_detect.cpp";
	constexpr auto kOpenCvFunction  = "FaceDetectorYNImpl::detect";
	constexpr auto kDnnEngineEnv    = "OPENCV_FORCE_DNN_ENGINE";

	class ScopedEnvironment final {
	public:
		explicit ScopedEnvironment(const char *name)
		    : name_(name) {
			if (const auto *value = std::getenv(name); value != nullptr) {
				original_value_ = value;
			}
		}

		~ScopedEnvironment() {
			if (original_value_.has_value()) {
				setenv(name_, original_value_->c_str(), 1);
			} else {
				unsetenv(name_);
			}
		}

		ScopedEnvironment(const ScopedEnvironment &)                     = delete;
		auto operator=(const ScopedEnvironment &) -> ScopedEnvironment & = delete;

	private:
		const char                *name_;
		std::optional<std::string> original_value_;
	};

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
		// OpenCV FaceRecognizerSF::alignCrop requires two adjacent cv::InputArray parameters.
		// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

	auto successful_backend() -> howdy::native::FaceModelTestAccess::Backend {
		return {
		    .check_readiness =
		        [](const std::filesystem::path &) -> howdy::native::OpenCvModelReadiness {
			    return howdy::native::OpenCvModelReadiness{
			        .status = howdy::native::OpenCvModelStatus::kOk,
			    };
		    },
		    .create_detector = [](const std::string &, const cv::Size &, float, float,
		                          int) -> cv::Ptr<FakeDetector> {
			    return cv::makePtr<FakeDetector>();
		    },
		    .create_recognizer = [](const std::string &) -> cv::Ptr<FakeRecognizer> {
			    return cv::makePtr<FakeRecognizer>();
		    },
		    .set_input_size = [](const cv::Size &) -> void {},
		    .detect         = [](const cv::Mat &, cv::Mat &faces) -> void {
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
	ok &= expect(
	    howdy::native::FaceModel::kYunetModel == howdy::native::kYunetModelDescriptor.filename &&
	        howdy::native::FaceModel::kSfaceModel == howdy::native::kSfaceModelDescriptor.filename,
	    "FaceModel paths map to manifest filenames");

	{
		std::string detector_path;
		std::string recognizer_path;
		auto        backend     = successful_backend();
		backend.create_detector = [&detector_path](const std::string &path, const cv::Size &, float,
		                                           float, int) -> cv::Ptr<FakeDetector> {
			detector_path = path;
			return cv::makePtr<FakeDetector>();
		};
		backend.create_recognizer =
		    [&recognizer_path](const std::string &path) -> cv::Ptr<FakeRecognizer> {
			recognizer_path = path;
			return cv::makePtr<FakeRecognizer>();
		};
		auto       model = howdy::native::FaceModelTestAccess::create(howdy::native::FaceConfig{},
		                                                              std::move(backend));
		const auto models_dir = howdy::native::resolve_models_dir();
		ok &= expect(model.ok(), "default config model initializes");
		ok &= expect(detector_path == (models_dir / howdy::native::FaceModel::kYunetModel).string(),
		             "default config uses canonical YuNet path");
		ok &=
		    expect(recognizer_path == (models_dir / howdy::native::FaceModel::kSfaceModel).string(),
		           "default config uses canonical SFace path");
	}

	{
		const ScopedEnvironment environment(kDnnEngineEnv);
		setenv(kDnnEngineEnv, "1", 1);

		std::string readiness_engine;
		std::string detector_engine;
		auto        backend = successful_backend();
		backend.check_readiness =
		    [&readiness_engine](
		        const std::filesystem::path &) -> howdy::native::OpenCvModelReadiness {
			if (const auto *value = std::getenv(kDnnEngineEnv); value != nullptr) {
				readiness_engine = value;
			}
			return howdy::native::OpenCvModelReadiness{
			    .status = howdy::native::OpenCvModelStatus::kOk,
			};
		};
		backend.create_detector = [&detector_engine](const std::string &, const cv::Size &, float,
		                                             float, int) -> cv::Ptr<FakeDetector> {
			if (const auto *value = std::getenv(kDnnEngineEnv); value != nullptr) {
				detector_engine = value;
			}
			return cv::makePtr<FakeDetector>();
		};
		auto model = howdy::native::FaceModelTestAccess::create(howdy::native::FaceConfig{},
		                                                        std::move(backend));
		ok &= expect(model.ok(), "engine override model initializes");
		const auto *engine = std::getenv(kDnnEngineEnv);
		ok &= expect(engine != nullptr && std::string(engine) == "2",
		             "New DNN graph engine overrides conflicting environment");
		ok &=
		    expect(readiness_engine == "1", "DNN engine remains unchanged during readiness checks");
		ok &= expect(detector_engine == "2", "New DNN graph engine is forced before factory call");
	}

	{
		auto backend            = successful_backend();
		backend.create_detector = [](const std::string &, const cv::Size &, float, float,
		                             int) -> cv::Ptr<cv::FaceDetectorYN> {
			throw_cv_error();
		};
		auto model = howdy::native::FaceModelTestAccess::create(howdy::native::FaceConfig{},
		                                                        std::move(backend));
		ok &= expect_init_failure(model,
		                          howdy::native::FaceModelErrorCategory::kDetectorInitialization,
		                          "Failed to initialize face detector");
	}

	{
		auto backend              = successful_backend();
		backend.create_recognizer = [](const std::string &) -> cv::Ptr<cv::FaceRecognizerSF> {
			throw_cv_error();
		};
		auto model = howdy::native::FaceModelTestAccess::create(howdy::native::FaceConfig{},
		                                                        std::move(backend));
		ok &= expect_init_failure(model,
		                          howdy::native::FaceModelErrorCategory::kRecognizerInitialization,
		                          "Failed to initialize face recognizer");
	}

	{
		auto backend            = successful_backend();
		backend.create_detector = [](const std::string &, const cv::Size &, float, float,
		                             int) -> cv::Ptr<cv::FaceDetectorYN> {
			return cv::Ptr<cv::FaceDetectorYN>{};
		};
		auto model = howdy::native::FaceModelTestAccess::create(howdy::native::FaceConfig{},
		                                                        std::move(backend));
		ok &= expect_init_failure(model,
		                          howdy::native::FaceModelErrorCategory::kDetectorInitialization,
		                          "Failed to initialize face detector");
	}

	{
		auto backend              = successful_backend();
		backend.create_recognizer = [](const std::string &) -> cv::Ptr<cv::FaceRecognizerSF> {
			return cv::Ptr<cv::FaceRecognizerSF>{};
		};
		auto model = howdy::native::FaceModelTestAccess::create(howdy::native::FaceConfig{},
		                                                        std::move(backend));
		ok &= expect_init_failure(model,
		                          howdy::native::FaceModelErrorCategory::kRecognizerInitialization,
		                          "Failed to initialize face recognizer");
	}

	{
		bool detector_created = false;
		auto backend          = successful_backend();
		backend.check_readiness =
		    [](const std::filesystem::path &) -> howdy::native::OpenCvModelReadiness {
			return howdy::native::OpenCvModelReadiness{
			    .status        = howdy::native::OpenCvModelStatus::kInvalid,
			    .error_message = std::string(kYunetSecretPath) + ": " + kRawError,
			};
		};
		backend.create_detector = [&detector_created](const std::string &, const cv::Size &, float,
		                                              float, int) -> cv::Ptr<FakeDetector> {
			detector_created = true;
			return cv::makePtr<FakeDetector>();
		};
		auto model = howdy::native::FaceModelTestAccess::create(howdy::native::FaceConfig{},
		                                                        std::move(backend));
		ok &= expect_init_failure(model, howdy::native::FaceModelErrorCategory::kModelNotReady,
		                          "Face model is not ready");
		ok &= expect(!detector_created, "first readiness failure prevents detector creation");
	}

	{
		bool recognizer_created = false;
		auto backend            = successful_backend();
		backend.check_readiness =
		    [](const std::filesystem::path &path) -> howdy::native::OpenCvModelReadiness {
			if (path.filename() == howdy::native::FaceModel::kSfaceModel) {
				return howdy::native::OpenCvModelReadiness{
				    .status        = howdy::native::OpenCvModelStatus::kInvalid,
				    .error_message = std::string(kSfaceSecretPath) + ": " + kRawError,
				};
			}
			return howdy::native::OpenCvModelReadiness{
			    .status = howdy::native::OpenCvModelStatus::kOk,
			};
		};
		backend.create_recognizer =
		    [&recognizer_created](const std::string &) -> cv::Ptr<FakeRecognizer> {
			recognizer_created = true;
			return cv::makePtr<FakeRecognizer>();
		};
		auto model = howdy::native::FaceModelTestAccess::create(howdy::native::FaceConfig{},
		                                                        std::move(backend));
		ok &= expect_init_failure(model, howdy::native::FaceModelErrorCategory::kModelNotReady,
		                          "Face model is not ready");
		ok &= expect(!recognizer_created, "second readiness failure prevents recognizer creation");
	}

	{
		auto backend           = successful_backend();
		backend.set_input_size = [](const cv::Size &) -> void {
			throw_cv_error();
		};
		auto model = howdy::native::FaceModelTestAccess::create(howdy::native::FaceConfig{},
		                                                        std::move(backend));
		ok &= expect(model.ok(), "setInputSize failure model initializes");
		ok &= expect_detection_failure(model);
	}

	{
		auto backend   = successful_backend();
		backend.detect = [](const cv::Mat &, cv::Mat &) -> void {
			throw_cv_error();
		};
		auto model = howdy::native::FaceModelTestAccess::create(howdy::native::FaceConfig{},
		                                                        std::move(backend));
		ok &= expect(model.ok(), "detector failure model initializes");
		ok &= expect_detection_failure(model);
	}

	{
		bool input_size_set    = false;
		bool detect_called     = false;
		auto backend           = successful_backend();
		backend.set_input_size = [&input_size_set](const cv::Size &size) -> void {
			input_size_set = size == cv::Size(640, 480);
		};
		backend.detect = [&detect_called](const cv::Mat &, cv::Mat &faces) -> void {
			detect_called = true;
			faces         = cv::Mat(1, 15, CV_32FC1);
			for (int column = 0; column < faces.cols; ++column) {
				faces.at<float>(0, column) = static_cast<float>(column + 1);
			}
		};
		auto       model  = howdy::native::FaceModelTestAccess::create(howdy::native::FaceConfig{},
		                                                               std::move(backend));
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
