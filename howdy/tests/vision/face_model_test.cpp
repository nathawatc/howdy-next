#include "config/runtime_config.hpp"
#include "config/runtime_paths.hpp"
#include "model_assets/model_file.hpp"
#include "model_assets/opencv_model_manifest.hpp"
#include "test_support.hpp"
#include "vision/face_detection.hpp"
#include "vision/face_encoding.hpp"
#include "vision/face_inference.hpp"
#include "vision/face_model.hpp"
#include "vision/face_model_test_access.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <stdlib.h>

#include <opencv2/core/cvstd_wrapper.hpp>
#include <opencv2/core/exception.hpp>
#include <opencv2/core/hal/interface.h>
#include <opencv2/core/mat.hpp>
#include <opencv2/core/types.hpp>
#include <opencv2/objdetect/face.hpp>

namespace {

	using howdy::test::Expect;
	constexpr auto kYunetSecretPath = "/secret/models/yunet.onnx";
	constexpr auto kSfaceSecretPath = "/secret/models/sface.onnx";
	constexpr auto kRawError        = "raw backend failure";
	constexpr auto kOpenCvSource    = "/opencv/modules/objdetect/src/face_detect.cpp";
	constexpr auto kOpenCvFunction  = "FaceDetectorYNImpl::detect";
	constexpr auto kDnnEngineEnv    = "OPENCV_FORCE_DNN_ENGINE";

	class ScopedEnvironment final {
	public:
		ScopedEnvironment(ScopedEnvironment &&)                     = delete;
		auto operator=(ScopedEnvironment &&) -> ScopedEnvironment & = delete;

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

	[[noreturn]] void ThrowCvError() {
		throw cv::Exception(cv::Error::StsError, kRawError, kOpenCvFunction, kOpenCvSource, 73);
	}

	[[noreturn]] void ThrowStdError() {
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
			ThrowStdError();
		}

		void feature([[maybe_unused]] cv::InputArray  aligned_img,
		             [[maybe_unused]] cv::OutputArray feature) override {}

		[[nodiscard]] auto match([[maybe_unused]] cv::InputArray face_feature1,
		                         [[maybe_unused]] cv::InputArray face_feature2,
		                         [[maybe_unused]] int dis_type) const -> double override {
			return 0.0;
		}
	};

	auto SuccessfulBackend() -> howdy::native::FaceModelTestAccess::Backend {
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

	auto Sanitized(const std::string &message) -> bool {
		return !message.empty() && !message.contains(kYunetSecretPath) &&
		       !message.contains(kSfaceSecretPath) && !message.contains(kRawError) &&
		       !message.contains(kOpenCvSource) && !message.contains(kOpenCvFunction);
	}

	auto ExpectInitFailure(const howdy::native::FaceModel &model,
	                       const std::string              &expected_message) -> bool {
		bool ok = true;
		ok &= Expect(!model.Ok(), "model initialization fails");
		ok &=
		    Expect(model.ErrorMessage() == expected_message, "initialization diagnostic is stable");
		ok &= Expect(Sanitized(model.ErrorMessage()), "initialization diagnostic is sanitized");
		return ok;
	}

	auto ExpectDetectionFailure(howdy::native::FaceModel &model) -> bool {
		const auto result = model.Detect(cv::Mat(480, 640, CV_8UC3, cv::Scalar(1, 2, 3)));
		bool       ok     = true;
		ok &= Expect(result.status == howdy::native::FaceDetectionStatus::kInferenceError,
		             "OpenCV exception maps to inference error");
		ok &= Expect(result.error_message == "Face detection failed",
		             "detection diagnostic is stable");
		ok &= Expect(Sanitized(result.error_message), "detection diagnostic is sanitized");
		return ok;
	}

	auto ExpectNotReadyInference(howdy::native::FaceModel &model) -> bool {
		bool ok = true;
		try {
			const auto detection = model.Detect(cv::Mat(480, 640, CV_8UC3, cv::Scalar(1, 2, 3)));
			ok &= Expect(detection.status == howdy::native::FaceDetectionStatus::kInferenceError,
			             "not-ready model detection returns inference error");
			ok &= Expect(detection.error_message == "Face model is not ready",
			             "not-ready model detection reports readiness failure");

			const auto encoding = model.Encode(cv::Mat(480, 640, CV_8UC3, cv::Scalar(1, 2, 3)), {});
			ok &= Expect(encoding.status == howdy::native::FaceEncodingStatus::kInferenceError,
			             "not-ready model encoding returns inference error");
			ok &= Expect(encoding.error_message == "Face model is not ready",
			             "not-ready model encoding reports readiness failure");
		} catch (...) {
			ok &= Expect(false, "not-ready model inference does not throw");
		}
		return ok;
	}

	auto ExpectEncodingStandardException() -> bool {
		auto backend              = SuccessfulBackend();
		backend.create_recognizer = [](const std::string &) -> cv::Ptr<ThrowingRecognizer> {
			return cv::makePtr<ThrowingRecognizer>();
		};
		auto model = howdy::native::FaceModelTestAccess::Create(howdy::native::DefaultFaceConfig(),
		                                                        std::move(backend));
		std::optional<howdy::native::FaceEncodingResult> result;
		bool                                             threw = false;
		try {
			result = model.Encode(cv::Mat(480, 640, CV_8UC3, cv::Scalar(1, 2, 3)), {});
		} catch (...) {
			threw = true;
		}
		bool ok = true;
		ok &= Expect(!threw, "SFace standard exception does not escape encoding");
		if (!threw) {
			ok &= Expect(result->status == howdy::native::FaceEncodingStatus::kInferenceError,
			             "SFace standard exception returns inference error");
			ok &= Expect(result->error_message ==
			                 "Face encoding failed while processing camera frame",
			             "SFace standard exception preserves encoding diagnostic");
		}
		return ok;
	}

	class ValidFeatureRecognizer final : public cv::FaceRecognizerSF {
	public:
		// OpenCV FaceRecognizerSF::alignCrop requires two adjacent cv::InputArray parameters.
		// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
		void alignCrop([[maybe_unused]] cv::InputArray src_img,
		               [[maybe_unused]] cv::InputArray face_box,
		               cv::OutputArray                 aligned) const override {
			const cv::Mat mat(112, 112, CV_8UC3, cv::Scalar(1, 1, 1));
			mat.copyTo(aligned);
		}

		void feature([[maybe_unused]] cv::InputArray aligned_img,
		             cv::OutputArray                 feature) override {
			const cv::Mat mat(1, 128, CV_32FC1, cv::Scalar(1.0F));
			mat.copyTo(feature);
		}

		[[nodiscard]] auto match([[maybe_unused]] cv::InputArray face_feature1,
		                         [[maybe_unused]] cv::InputArray face_feature2,
		                         [[maybe_unused]] int dis_type) const -> double override {
			return 0.0;
		}
	};

	enum class WarmFailure : std::uint8_t {
		kNone,
		kEmptyAlignment,
		kEmptyFeature,
		kInvalidFeature,
		kCvException,
		kStdException
	};

	struct WarmObservations {
		int         alignments = 0;
		int         features   = 0;
		cv::Size    input_size;
		int         input_type = -1;
		cv::Mat     face_row;
		WarmFailure failure = WarmFailure::kNone;
	};

	class ObservedWarmRecognizer final : public cv::FaceRecognizerSF {
	public:
		explicit ObservedWarmRecognizer(WarmObservations &observations)
		    : observations_(&observations) {}

		// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
		void alignCrop(cv::InputArray src_img, cv::InputArray face_box,
		               cv::OutputArray aligned) const override {
			++observations_->alignments;
			observations_->input_size = src_img.getMat().size();
			observations_->input_type = src_img.getMat().type();
			observations_->face_row   = face_box.getMat().clone();
			if (observations_->alignments == 1) {
				if (observations_->failure == WarmFailure::kEmptyAlignment) {
					return;
				}
				if (observations_->failure == WarmFailure::kCvException) {
					ThrowCvError();
				}
				if (observations_->failure == WarmFailure::kStdException) {
					ThrowStdError();
				}
			}
			src_img.getMat().copyTo(aligned);
		}

		void feature([[maybe_unused]] cv::InputArray aligned_img,
		             cv::OutputArray                 feature) override {
			++observations_->features;
			if (observations_->features == 1 &&
			    observations_->failure == WarmFailure::kEmptyFeature) {
				return;
			}
			const int width = observations_->features == 1 &&
			                          observations_->failure == WarmFailure::kInvalidFeature
			                      ? 127
			                      : 128;
			cv::Mat(1, width, CV_32FC1, cv::Scalar(1.0F)).copyTo(feature);
		}

		[[nodiscard]] auto match([[maybe_unused]] cv::InputArray face_feature1,
		                         [[maybe_unused]] cv::InputArray face_feature2,
		                         [[maybe_unused]] int dis_type) const -> double override {
			return 0.0;
		}

	private:
		WarmObservations *observations_;
	};

	auto TestRecognizerWarmUp() -> bool {
		bool ok = true;
		for (const auto failure : {WarmFailure::kNone, WarmFailure::kEmptyAlignment,
		                           WarmFailure::kEmptyFeature, WarmFailure::kInvalidFeature,
		                           WarmFailure::kCvException, WarmFailure::kStdException}) {
			WarmObservations observations{.failure = failure};
			int              detect_calls     = 0;
			int              input_size_calls = 0;
			auto             backend          = SuccessfulBackend();
			backend.create_recognizer =
			    [&observations](const std::string &) -> cv::Ptr<cv::FaceRecognizerSF> {
				return {std::make_shared<ObservedWarmRecognizer>(observations)};
			};
			backend.detect = [&detect_calls](const cv::Mat &, cv::Mat &) -> void {
				++detect_calls;
			};
			backend.set_input_size = [&input_size_calls](const cv::Size &) -> void {
				++input_size_calls;
			};
			auto model = howdy::native::FaceModelTestAccess::Create(
			    howdy::native::DefaultFaceConfig(), std::move(backend));
			const auto initial_error = model.ErrorMessage();
			ok &= Expect(model.Ok() && observations.alignments == 0 && observations.features == 0,
			             "model construction does not warm SFace");
			const bool succeeded = model.WarmUpRecognizer();
			ok &= Expect(succeeded == (failure == WarmFailure::kNone),
			             "warm-up reports synthetic inference outcome");
			const int alignments = observations.alignments;
			const int features   = observations.features;
			ok &= Expect(!model.WarmUpRecognizer() && observations.alignments == alignments &&
			                 observations.features == features,
			             "second warm-up attempt performs no work after success or failure");
			ok &=
			    Expect(observations.alignments == 1 &&
				           observations.features == (failure == WarmFailure::kEmptyAlignment ||
				                                             failure == WarmFailure::kCvException ||
				                                             failure == WarmFailure::kStdException
			                                             ? 0
														 : 1),
				       "warm-up invokes alignment and feature once when reached");
			ok &= Expect(observations.input_size == cv::Size(112, 112) &&
			                 observations.input_type == CV_8UC3,
			             "warm-up uses fixed 112 by 112 BGR input");
			ok &= Expect(observations.face_row.rows == 1 && observations.face_row.cols == 15 &&
			                 observations.face_row.type() == CV_32FC1,
			             "warm-up uses normal SFace face-row encoding path");
			if (observations.face_row.rows == 1 && observations.face_row.cols == 15) {
				constexpr std::array landmarks = {38.2946F, 51.6963F, 73.5318F, 51.5014F, 56.0252F,
				                                  71.7366F, 41.5493F, 92.3655F, 70.7299F, 92.2041F};
				for (int index = 0; index < 10; ++index) {
					ok &= Expect(observations.face_row.at<float>(0, 4 + index) ==
					                 landmarks.at(static_cast<std::size_t>(index)),
					             "warm-up passes canonical landmark through Encode");
				}
			}
			ok &= Expect(detect_calls == 0 && input_size_calls == 0,
			             "warm-up does not use YuNet or change detector input size");
			ok &= Expect(model.Ok() && model.ErrorMessage() == initial_error,
			             "optional warm-up failure does not invalidate model");
			const auto real = model.Encode(cv::Mat(112, 112, CV_8UC3, cv::Scalar(2, 3, 4)), {});
			ok &= Expect(howdy::native::FaceEncodingSucceeded(real),
			             "real Encode remains authoritative after warm-up outcome");
		}
		return ok;
	}

	auto TestFaceModelInferenceOperations() -> bool {
		auto backend   = SuccessfulBackend();
		backend.detect = [](const cv::Mat &, cv::Mat &faces) -> void {
			faces = cv::Mat(1, 15, CV_32FC1);
			for (int column = 0; column < faces.cols; ++column) {
				faces.at<float>(0, column) = static_cast<float>(column + 1);
			}
		};
		backend.create_recognizer = [](const std::string &) -> cv::Ptr<cv::FaceRecognizerSF> {
			return cv::makePtr<ValidFeatureRecognizer>();
		};

		auto model = howdy::native::FaceModelTestAccess::Create(howdy::native::DefaultFaceConfig(),
		                                                        std::move(backend));
		auto ops   = howdy::native::FaceModelInferenceOperations(model);
		bool ok    = true;
		ok &= Expect(howdy::native::FaceInferenceComplete(ops),
		             "FaceModelInferenceOperations produces complete operations");
		ok &= Expect(howdy::native::FaceInferenceDetectionReady(ops),
		             "FaceModelInferenceOperations is detection ready");
		ok &= Expect(ops.context == &model, "operations context points to model");

		const cv::Mat gray(60, 60, CV_8UC1, cv::Scalar(100));
		const auto    prepared = ops.prepare_frame(ops.context, gray);
		ok &= Expect(prepared.channels() == 3, "operations prepare_frame produces BGR frame");

		const auto detection = ops.detect_faces(ops.context, prepared);
		ok &= Expect(howdy::native::FaceDetectionSucceeded(detection) &&
		                 detection.detections.size() == 1,
		             "operations detect_faces invokes model");

		if (!howdy::native::FaceDetectionSucceeded(detection) || detection.detections.empty()) {
			return false;
		}

		const auto encoding = ops.encode_face(ops.context, prepared, detection.detections.at(0));
		ok &= Expect(howdy::native::FaceEncodingSucceeded(encoding) && !encoding.encoding.empty(),
		             "operations encode_face invokes model");

		if (!howdy::native::FaceEncodingSucceeded(encoding) || encoding.encoding.empty()) {
			return false;
		}

		return ok;
	}

	auto TestDnnEngineEnvironment() -> bool {
		bool ok = true;
		for (const bool preconfigure : {false, true}) {
			const ScopedEnvironment environment(kDnnEngineEnv);
			setenv(kDnnEngineEnv, "1", 1);
			if (preconfigure) {
				ok &= Expect(howdy::native::ConfigureFaceModelEnvironment(),
				             "DNN engine can be configured before parallel initialization");
			}

			std::string readiness_engine;
			std::string detector_engine;
			auto        backend = SuccessfulBackend();
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
			backend.create_detector = [&detector_engine](const std::string &, const cv::Size &,
			                                             float, float,
			                                             int) -> cv::Ptr<FakeDetector> {
				if (const auto *value = std::getenv(kDnnEngineEnv); value != nullptr) {
					detector_engine = value;
				}
				return cv::makePtr<FakeDetector>();
			};
			auto model = howdy::native::FaceModelTestAccess::Create(
			    howdy::native::DefaultFaceConfig(), std::move(backend));
			ok &= Expect(model.Ok(), "engine override model initializes");
			const auto *engine = std::getenv(kDnnEngineEnv);
			ok &= Expect(engine != nullptr && std::string(engine) == "2",
			             "New DNN graph engine overrides conflicting environment");
			ok &= Expect(readiness_engine == (preconfigure ? "2" : "1"),
			             "readiness checks preserve the preconfigured DNN engine");
			ok &= Expect(detector_engine == "2",
			             "New DNN graph engine is forced before factory call");
		}
		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= Expect(
	    howdy::native::FaceModel::kYunetModel == howdy::native::kYunetModelDescriptor.filename &&
	        howdy::native::FaceModel::kSfaceModel == howdy::native::kSfaceModelDescriptor.filename,
	    "FaceModel paths map to manifest filenames");

	{
		std::string detector_path;
		std::string recognizer_path;
		auto        backend     = SuccessfulBackend();
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
		auto model = howdy::native::FaceModelTestAccess::Create(howdy::native::DefaultFaceConfig(),
		                                                        std::move(backend));
		const auto models_dir = howdy::native::ResolveModelsDir();
		ok &= Expect(model.Ok(), "default config model initializes");
		ok &= Expect(detector_path == (models_dir / howdy::native::FaceModel::kYunetModel).string(),
		             "default config uses canonical YuNet path");
		ok &=
		    Expect(recognizer_path == (models_dir / howdy::native::FaceModel::kSfaceModel).string(),
			       "default config uses canonical SFace path");
	}

	ok &= TestDnnEngineEnvironment();

	{
		auto backend            = SuccessfulBackend();
		backend.create_detector = [](const std::string &, const cv::Size &, float, float,
		                             int) -> cv::Ptr<cv::FaceDetectorYN> {
			ThrowCvError();
		};
		auto model = howdy::native::FaceModelTestAccess::Create(howdy::native::DefaultFaceConfig(),
		                                                        std::move(backend));
		ok &= ExpectInitFailure(model, "Failed to initialize face detector");
	}

	{
		auto backend              = SuccessfulBackend();
		backend.create_recognizer = [](const std::string &) -> cv::Ptr<cv::FaceRecognizerSF> {
			ThrowCvError();
		};
		auto model = howdy::native::FaceModelTestAccess::Create(howdy::native::DefaultFaceConfig(),
		                                                        std::move(backend));
		ok &= ExpectInitFailure(model, "Failed to initialize face recognizer");
	}

	{
		auto backend = SuccessfulBackend();
		backend.check_readiness =
		    [](const std::filesystem::path &) -> howdy::native::OpenCvModelReadiness {
			ThrowStdError();
		};
		bool threw = false;
		try {
			auto model = howdy::native::FaceModelTestAccess::Create(
			    howdy::native::DefaultFaceConfig(), std::move(backend));
			ok &= ExpectInitFailure(model, "Face model is not ready");
		} catch (...) {
			threw = true;
		}
		ok &= Expect(!threw, "readiness standard exception fails model construction cleanly");
	}

	{
		auto backend            = SuccessfulBackend();
		backend.create_detector = [](const std::string &, const cv::Size &, float, float,
		                             int) -> cv::Ptr<FakeDetector> {
			ThrowStdError();
		};
		bool threw = false;
		try {
			auto model = howdy::native::FaceModelTestAccess::Create(
			    howdy::native::DefaultFaceConfig(), std::move(backend));
			ok &= ExpectInitFailure(model, "Failed to initialize face detector");
		} catch (...) {
			threw = true;
		}
		ok &= Expect(!threw, "detector standard exception fails model construction cleanly");
	}

	{
		auto backend              = SuccessfulBackend();
		backend.create_recognizer = [](const std::string &) -> cv::Ptr<FakeRecognizer> {
			ThrowStdError();
		};
		bool threw = false;
		try {
			auto model = howdy::native::FaceModelTestAccess::Create(
			    howdy::native::DefaultFaceConfig(), std::move(backend));
			ok &= ExpectInitFailure(model, "Failed to initialize face recognizer");
		} catch (...) {
			threw = true;
		}
		ok &= Expect(!threw, "recognizer standard exception fails model construction cleanly");
	}

	ok &= ExpectEncodingStandardException();

	{
		auto backend            = SuccessfulBackend();
		backend.create_detector = [](const std::string &, const cv::Size &, float, float,
		                             int) -> cv::Ptr<cv::FaceDetectorYN> {
			return cv::Ptr<cv::FaceDetectorYN>{};
		};
		auto model = howdy::native::FaceModelTestAccess::Create(howdy::native::DefaultFaceConfig(),
		                                                        std::move(backend));
		ok &= ExpectInitFailure(model, "Failed to initialize face detector");
	}

	{
		auto backend              = SuccessfulBackend();
		backend.create_recognizer = [](const std::string &) -> cv::Ptr<cv::FaceRecognizerSF> {
			return cv::Ptr<cv::FaceRecognizerSF>{};
		};
		auto model = howdy::native::FaceModelTestAccess::Create(howdy::native::DefaultFaceConfig(),
		                                                        std::move(backend));
		ok &= ExpectInitFailure(model, "Failed to initialize face recognizer");
	}

	{
		bool detector_created = false;
		auto backend          = SuccessfulBackend();
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
		auto model = howdy::native::FaceModelTestAccess::Create(howdy::native::DefaultFaceConfig(),
		                                                        std::move(backend));
		ok &= ExpectInitFailure(model, "Face model is not ready");
		ok &= Expect(!detector_created, "first readiness failure prevents detector creation");
		ok &= ExpectNotReadyInference(model);
	}

	{
		bool recognizer_created = false;
		auto backend            = SuccessfulBackend();
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
		auto model = howdy::native::FaceModelTestAccess::Create(howdy::native::DefaultFaceConfig(),
		                                                        std::move(backend));
		ok &= ExpectInitFailure(model, "Face model is not ready");
		ok &= Expect(!recognizer_created, "second readiness failure prevents recognizer creation");
	}

	{
		auto backend            = SuccessfulBackend();
		backend.check_readiness = nullptr;
		bool threw              = false;
		try {
			auto model = howdy::native::FaceModelTestAccess::Create(
			    howdy::native::DefaultFaceConfig(), std::move(backend));
			ok &= ExpectInitFailure(model, "Face model is not ready");
		} catch (...) {
			threw = true;
		}
		ok &= Expect(!threw, "missing readiness callback fails model construction cleanly");
	}

	{
		auto backend            = SuccessfulBackend();
		backend.create_detector = nullptr;
		bool threw              = false;
		try {
			auto model = howdy::native::FaceModelTestAccess::Create(
			    howdy::native::DefaultFaceConfig(), std::move(backend));
			ok &= ExpectInitFailure(model, "Failed to initialize face detector");
		} catch (...) {
			threw = true;
		}
		ok &= Expect(!threw, "missing detector callback fails model construction cleanly");
	}

	{
		auto backend              = SuccessfulBackend();
		backend.create_recognizer = nullptr;
		bool threw                = false;
		try {
			auto model = howdy::native::FaceModelTestAccess::Create(
			    howdy::native::DefaultFaceConfig(), std::move(backend));
			ok &= ExpectInitFailure(model, "Failed to initialize face recognizer");
		} catch (...) {
			threw = true;
		}
		ok &= Expect(!threw, "missing recognizer callback fails model construction cleanly");
	}

	{
		auto backend           = SuccessfulBackend();
		backend.set_input_size = nullptr;
		auto model = howdy::native::FaceModelTestAccess::Create(howdy::native::DefaultFaceConfig(),
		                                                        std::move(backend));
		std::optional<howdy::native::FaceDetectionResult> result;
		bool                                              threw = false;
		try {
			result = model.Detect(cv::Mat(480, 640, CV_8UC3, cv::Scalar(1, 2, 3)));
		} catch (...) {
			threw = true;
		}
		ok &= Expect(!threw, "missing input-size callback does not throw");
		if (!threw) {
			ok &= Expect(result->status == howdy::native::FaceDetectionStatus::kInferenceError,
			             "missing input-size callback returns inference error");
		}
	}

	{
		auto backend   = SuccessfulBackend();
		backend.detect = nullptr;
		auto model = howdy::native::FaceModelTestAccess::Create(howdy::native::DefaultFaceConfig(),
		                                                        std::move(backend));
		std::optional<howdy::native::FaceDetectionResult> result;
		bool                                              threw = false;
		try {
			result = model.Detect(cv::Mat(320, 320, CV_8UC3, cv::Scalar(1, 2, 3)));
		} catch (...) {
			threw = true;
		}
		ok &= Expect(!threw, "missing detection callback does not throw");
		if (!threw) {
			ok &= Expect(result->status == howdy::native::FaceDetectionStatus::kInferenceError,
			             "missing detection callback returns inference error");
		}
	}

	{
		auto backend   = SuccessfulBackend();
		backend.detect = [](const cv::Mat &, cv::Mat &) -> void {
			ThrowStdError();
		};
		auto model = howdy::native::FaceModelTestAccess::Create(howdy::native::DefaultFaceConfig(),
		                                                        std::move(backend));
		std::optional<howdy::native::FaceDetectionResult> result;
		bool                                              threw = false;
		try {
			result = model.Detect(cv::Mat(320, 320, CV_8UC3, cv::Scalar(1, 2, 3)));
		} catch (...) {
			threw = true;
		}
		ok &= Expect(!threw, "backend standard exception does not escape detection");
		if (!threw) {
			ok &= Expect(result->status == howdy::native::FaceDetectionStatus::kInferenceError,
			             "backend standard exception returns inference error");
		}
	}

	{
		bool detect_called = false;
		auto backend       = SuccessfulBackend();
		backend.detect     = [&detect_called](const cv::Mat &, cv::Mat &) -> void {
			detect_called = true;
		};
		auto model = howdy::native::FaceModelTestAccess::Create(howdy::native::DefaultFaceConfig(),
		                                                        std::move(backend));
		const auto result = model.Detect(cv::Mat{});
		ok &= Expect(result.status == howdy::native::FaceDetectionStatus::kInferenceError,
		             "empty detection input returns inference error");
		ok &= Expect(!detect_called, "empty detection input skips backend");

		const auto invalid = model.Detect(cv::Mat(10, 10, CV_16UC3, cv::Scalar(1, 2, 3)));
		ok &= Expect(invalid.status == howdy::native::FaceDetectionStatus::kInferenceError,
		             "invalid detection input returns inference error");
		ok &= Expect(!detect_called, "invalid detection input skips backend");
	}

	{
		auto model = howdy::native::FaceModelTestAccess::Create(howdy::native::DefaultFaceConfig(),
		                                                        SuccessfulBackend());
		const auto result = model.Encode(cv::Mat{}, {});
		ok &= Expect(result.status == howdy::native::FaceEncodingStatus::kInferenceError,
		             "empty encoding input returns inference error");
		ok &= Expect(result.error_message == "Face encoding failed: invalid input frame",
		             "empty encoding input reports validation failure");
	}

	{
		auto backend           = SuccessfulBackend();
		backend.set_input_size = [](const cv::Size &) -> void {
			ThrowCvError();
		};
		auto model = howdy::native::FaceModelTestAccess::Create(howdy::native::DefaultFaceConfig(),
		                                                        std::move(backend));
		ok &= Expect(model.Ok(), "setInputSize failure model initializes");
		ok &= ExpectDetectionFailure(model);
	}

	{
		auto backend   = SuccessfulBackend();
		backend.detect = [](const cv::Mat &, cv::Mat &) -> void {
			ThrowCvError();
		};
		auto model = howdy::native::FaceModelTestAccess::Create(howdy::native::DefaultFaceConfig(),
		                                                        std::move(backend));
		ok &= Expect(model.Ok(), "detector failure model initializes");
		ok &= ExpectDetectionFailure(model);
	}

	{
		bool input_size_set    = false;
		bool detect_called     = false;
		auto backend           = SuccessfulBackend();
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
		auto model = howdy::native::FaceModelTestAccess::Create(howdy::native::DefaultFaceConfig(),
		                                                        std::move(backend));
		const auto result = model.Detect(cv::Mat(480, 640, CV_8UC3, cv::Scalar(1, 2, 3)));
		ok &= Expect(model.Ok(), "success backend initializes");
		ok &= Expect(input_size_set, "success path updates detector input size");
		ok &= Expect(detect_called, "success path invokes detector");
		ok &= Expect(howdy::native::FaceDetectionSucceeded(result) && result.detections.size() == 1,
		             "success path preserves parsed detection");
		if (result.detections.size() == 1) {
			ok &= Expect(result.detections.front().box == cv::Rect2f(1, 2, 3, 4),
			             "success path preserves detection fields");
		}
	}

	ok &= TestFaceModelInferenceOperations();
	ok &= TestRecognizerWarmUp();

	return ok ? 0 : 1;
}
