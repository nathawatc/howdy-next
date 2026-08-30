#include "config/runtime_paths.hpp"
#include "test_support.hpp"
#include "vision/face_model.hpp"
#include "vision/face_model_test_access.hpp"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

	using howdy::test::expect;
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

	[[noreturn]] void throw_cv_error() {
		throw cv::Exception(cv::Error::StsError, kRawError, kOpenCvFunction, kOpenCvSource, 73);
	}

	[[noreturn]] void throw_std_error() {
		throw std::runtime_error(kRawError);
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

	class ThrowingRecognizer final : public cv::FaceRecognizerSF {
	public:
		// OpenCV FaceRecognizerSF::alignCrop requires two adjacent cv::InputArray parameters.
		// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
		void alignCrop([[maybe_unused]] cv::InputArray  src_img,
		               [[maybe_unused]] cv::InputArray  face_box,
		               [[maybe_unused]] cv::OutputArray aligned) const override {
			throw_std_error();
		}

		void feature([[maybe_unused]] cv::InputArray  aligned_img,
		             [[maybe_unused]] cv::OutputArray feature) override {}

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

	auto expect_not_ready_inference(howdy::native::FaceModel &model) -> bool {
		bool ok = true;
		try {
			const auto detection = model.detect(cv::Mat(480, 640, CV_8UC3, cv::Scalar(1, 2, 3)));
			ok &= expect(detection.status == howdy::native::FaceDetectionStatus::kInferenceError,
			             "not-ready model detection returns inference error");
			ok &= expect(detection.error_message == "Face model is not ready",
			             "not-ready model detection reports readiness failure");

			const auto encoding = model.encode(cv::Mat(480, 640, CV_8UC3, cv::Scalar(1, 2, 3)), {});
			ok &= expect(encoding.status == howdy::native::FaceEncodingStatus::kInferenceError,
			             "not-ready model encoding returns inference error");
			ok &= expect(encoding.error_message == "Face model is not ready",
			             "not-ready model encoding reports readiness failure");
		} catch (...) {
			ok &= expect(false, "not-ready model inference does not throw");
		}
		return ok;
	}

	auto expect_encoding_standard_exception() -> bool {
		auto backend              = successful_backend();
		backend.create_recognizer = [](const std::string &) -> cv::Ptr<ThrowingRecognizer> {
			return cv::makePtr<ThrowingRecognizer>();
		};
		auto model = howdy::native::FaceModelTestAccess::create(
		    howdy::native::default_face_config(), std::move(backend));
		std::optional<howdy::native::FaceEncodingResult> result;
		bool                                             threw = false;
		try {
			result = model.encode(cv::Mat(480, 640, CV_8UC3, cv::Scalar(1, 2, 3)), {});
		} catch (...) {
			threw = true;
		}
		bool ok = true;
		ok &= expect(!threw, "SFace standard exception does not escape encoding");
		if (!threw) {
			ok &= expect(result->status == howdy::native::FaceEncodingStatus::kInferenceError,
			             "SFace standard exception returns inference error");
			ok &= expect(result->error_message ==
			                 "Face encoding failed while processing camera frame",
			             "SFace standard exception preserves encoding diagnostic");
		}
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
		auto model = howdy::native::FaceModelTestAccess::create(
		    howdy::native::default_face_config(), std::move(backend));
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
		auto model = howdy::native::FaceModelTestAccess::create(
		    howdy::native::default_face_config(), std::move(backend));
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
		auto model = howdy::native::FaceModelTestAccess::create(
		    howdy::native::default_face_config(), std::move(backend));
		ok &= expect_init_failure(model,
		                          howdy::native::FaceModelErrorCategory::kDetectorInitialization,
		                          "Failed to initialize face detector");
	}

	{
		auto backend              = successful_backend();
		backend.create_recognizer = [](const std::string &) -> cv::Ptr<cv::FaceRecognizerSF> {
			throw_cv_error();
		};
		auto model = howdy::native::FaceModelTestAccess::create(
		    howdy::native::default_face_config(), std::move(backend));
		ok &= expect_init_failure(model,
		                          howdy::native::FaceModelErrorCategory::kRecognizerInitialization,
		                          "Failed to initialize face recognizer");
	}

	{
		auto backend = successful_backend();
		backend.check_readiness =
		    [](const std::filesystem::path &) -> howdy::native::OpenCvModelReadiness {
			throw_std_error();
		};
		bool threw = false;
		try {
			auto model = howdy::native::FaceModelTestAccess::create(
			    howdy::native::default_face_config(), std::move(backend));
			ok &= expect_init_failure(model, howdy::native::FaceModelErrorCategory::kModelNotReady,
			                          "Face model is not ready");
		} catch (...) {
			threw = true;
		}
		ok &= expect(!threw, "readiness standard exception fails model construction cleanly");
	}

	{
		auto backend            = successful_backend();
		backend.create_detector = [](const std::string &, const cv::Size &, float, float,
		                             int) -> cv::Ptr<FakeDetector> {
			throw_std_error();
		};
		bool threw = false;
		try {
			auto model = howdy::native::FaceModelTestAccess::create(
			    howdy::native::default_face_config(), std::move(backend));
			ok &= expect_init_failure(
			    model, howdy::native::FaceModelErrorCategory::kDetectorInitialization,
			    "Failed to initialize face detector");
		} catch (...) {
			threw = true;
		}
		ok &= expect(!threw, "detector standard exception fails model construction cleanly");
	}

	{
		auto backend              = successful_backend();
		backend.create_recognizer = [](const std::string &) -> cv::Ptr<FakeRecognizer> {
			throw_std_error();
		};
		bool threw = false;
		try {
			auto model = howdy::native::FaceModelTestAccess::create(
			    howdy::native::default_face_config(), std::move(backend));
			ok &= expect_init_failure(
			    model, howdy::native::FaceModelErrorCategory::kRecognizerInitialization,
			    "Failed to initialize face recognizer");
		} catch (...) {
			threw = true;
		}
		ok &= expect(!threw, "recognizer standard exception fails model construction cleanly");
	}

	ok &= expect_encoding_standard_exception();

	{
		auto backend            = successful_backend();
		backend.create_detector = [](const std::string &, const cv::Size &, float, float,
		                             int) -> cv::Ptr<cv::FaceDetectorYN> {
			return cv::Ptr<cv::FaceDetectorYN>{};
		};
		auto model = howdy::native::FaceModelTestAccess::create(
		    howdy::native::default_face_config(), std::move(backend));
		ok &= expect_init_failure(model,
		                          howdy::native::FaceModelErrorCategory::kDetectorInitialization,
		                          "Failed to initialize face detector");
	}

	{
		auto backend              = successful_backend();
		backend.create_recognizer = [](const std::string &) -> cv::Ptr<cv::FaceRecognizerSF> {
			return cv::Ptr<cv::FaceRecognizerSF>{};
		};
		auto model = howdy::native::FaceModelTestAccess::create(
		    howdy::native::default_face_config(), std::move(backend));
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
		auto model = howdy::native::FaceModelTestAccess::create(
		    howdy::native::default_face_config(), std::move(backend));
		ok &= expect_init_failure(model, howdy::native::FaceModelErrorCategory::kModelNotReady,
		                          "Face model is not ready");
		ok &= expect(!detector_created, "first readiness failure prevents detector creation");
		ok &= expect_not_ready_inference(model);
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
		auto model = howdy::native::FaceModelTestAccess::create(
		    howdy::native::default_face_config(), std::move(backend));
		ok &= expect_init_failure(model, howdy::native::FaceModelErrorCategory::kModelNotReady,
		                          "Face model is not ready");
		ok &= expect(!recognizer_created, "second readiness failure prevents recognizer creation");
	}

	{
		auto backend            = successful_backend();
		backend.check_readiness = nullptr;
		bool threw              = false;
		try {
			auto model = howdy::native::FaceModelTestAccess::create(
			    howdy::native::default_face_config(), std::move(backend));
			ok &= expect_init_failure(model, howdy::native::FaceModelErrorCategory::kModelNotReady,
			                          "Face model is not ready");
		} catch (...) {
			threw = true;
		}
		ok &= expect(!threw, "missing readiness callback fails model construction cleanly");
	}

	{
		auto backend            = successful_backend();
		backend.create_detector = nullptr;
		bool threw              = false;
		try {
			auto model = howdy::native::FaceModelTestAccess::create(
			    howdy::native::default_face_config(), std::move(backend));
			ok &= expect_init_failure(
			    model, howdy::native::FaceModelErrorCategory::kDetectorInitialization,
			    "Failed to initialize face detector");
		} catch (...) {
			threw = true;
		}
		ok &= expect(!threw, "missing detector callback fails model construction cleanly");
	}

	{
		auto backend              = successful_backend();
		backend.create_recognizer = nullptr;
		bool threw                = false;
		try {
			auto model = howdy::native::FaceModelTestAccess::create(
			    howdy::native::default_face_config(), std::move(backend));
			ok &= expect_init_failure(
			    model, howdy::native::FaceModelErrorCategory::kRecognizerInitialization,
			    "Failed to initialize face recognizer");
		} catch (...) {
			threw = true;
		}
		ok &= expect(!threw, "missing recognizer callback fails model construction cleanly");
	}

	{
		auto backend           = successful_backend();
		backend.set_input_size = nullptr;
		auto model             = howdy::native::FaceModelTestAccess::create(
		    howdy::native::default_face_config(), std::move(backend));
		std::optional<howdy::native::FaceDetectionResult> result;
		bool                                              threw = false;
		try {
			result = model.detect(cv::Mat(480, 640, CV_8UC3, cv::Scalar(1, 2, 3)));
		} catch (...) {
			threw = true;
		}
		ok &= expect(!threw, "missing input-size callback does not throw");
		if (!threw) {
			ok &= expect(result->status == howdy::native::FaceDetectionStatus::kInferenceError,
			             "missing input-size callback returns inference error");
		}
	}

	{
		auto backend   = successful_backend();
		backend.detect = nullptr;
		auto model     = howdy::native::FaceModelTestAccess::create(
		    howdy::native::default_face_config(), std::move(backend));
		std::optional<howdy::native::FaceDetectionResult> result;
		bool                                              threw = false;
		try {
			result = model.detect(cv::Mat(320, 320, CV_8UC3, cv::Scalar(1, 2, 3)));
		} catch (...) {
			threw = true;
		}
		ok &= expect(!threw, "missing detection callback does not throw");
		if (!threw) {
			ok &= expect(result->status == howdy::native::FaceDetectionStatus::kInferenceError,
			             "missing detection callback returns inference error");
		}
	}

	{
		auto backend   = successful_backend();
		backend.detect = [](const cv::Mat &, cv::Mat &) -> void {
			throw_std_error();
		};
		auto model = howdy::native::FaceModelTestAccess::create(
		    howdy::native::default_face_config(), std::move(backend));
		std::optional<howdy::native::FaceDetectionResult> result;
		bool                                              threw = false;
		try {
			result = model.detect(cv::Mat(320, 320, CV_8UC3, cv::Scalar(1, 2, 3)));
		} catch (...) {
			threw = true;
		}
		ok &= expect(!threw, "backend standard exception does not escape detection");
		if (!threw) {
			ok &= expect(result->status == howdy::native::FaceDetectionStatus::kInferenceError,
			             "backend standard exception returns inference error");
		}
	}

	{
		bool detect_called = false;
		auto backend       = successful_backend();
		backend.detect     = [&detect_called](const cv::Mat &, cv::Mat &) -> void {
			detect_called = true;
		};
		auto model = howdy::native::FaceModelTestAccess::create(
		    howdy::native::default_face_config(), std::move(backend));
		const auto result = model.detect(cv::Mat{});
		ok &= expect(result.status == howdy::native::FaceDetectionStatus::kInferenceError,
		             "empty detection input returns inference error");
		ok &= expect(!detect_called, "empty detection input skips backend");

		const auto invalid = model.detect(cv::Mat(10, 10, CV_16UC3, cv::Scalar(1, 2, 3)));
		ok &= expect(invalid.status == howdy::native::FaceDetectionStatus::kInferenceError,
		             "invalid detection input returns inference error");
		ok &= expect(!detect_called, "invalid detection input skips backend");
	}

	{
		auto model = howdy::native::FaceModelTestAccess::create(
		    howdy::native::default_face_config(), successful_backend());
		const auto result = model.encode(cv::Mat{}, {});
		ok &= expect(result.status == howdy::native::FaceEncodingStatus::kInferenceError,
		             "empty encoding input returns inference error");
		ok &= expect(result.error_message == "Face encoding failed: invalid input frame",
		             "empty encoding input reports validation failure");
	}

	{
		auto backend           = successful_backend();
		backend.set_input_size = [](const cv::Size &) -> void {
			throw_cv_error();
		};
		auto model = howdy::native::FaceModelTestAccess::create(
		    howdy::native::default_face_config(), std::move(backend));
		ok &= expect(model.ok(), "setInputSize failure model initializes");
		ok &= expect_detection_failure(model);
	}

	{
		auto backend   = successful_backend();
		backend.detect = [](const cv::Mat &, cv::Mat &) -> void {
			throw_cv_error();
		};
		auto model = howdy::native::FaceModelTestAccess::create(
		    howdy::native::default_face_config(), std::move(backend));
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
		auto model = howdy::native::FaceModelTestAccess::create(
		    howdy::native::default_face_config(), std::move(backend));
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
