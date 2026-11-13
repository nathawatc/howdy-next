#include "vision/face_model.hpp"

#include "config/runtime_config.hpp"
#include "config/runtime_paths.hpp"
#include "model_assets/model_file.hpp"
#include "support/face_metric.hpp"
#include "vision/face_detection.hpp"
#include "vision/face_encoding.hpp"
#include "vision/face_encoding/internal.hpp"
#include "vision/face_inference.hpp"
#include "vision/face_model/internal.hpp"
#include "vision/frame_validation.hpp"

#include <array>
#include <exception>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>

#include <stdlib.h>

#include <opencv2/core/cvstd_wrapper.hpp>
#include <opencv2/core/exception.hpp>
#include <opencv2/core/hal/interface.h>
#include <opencv2/core/mat.hpp>
#include <opencv2/core/types.hpp>
#include <opencv2/core/version.hpp>
#include <opencv2/dnn/dnn.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect/face.hpp>

#include <sys/types.h>

namespace howdy::native {
	namespace {
		constexpr auto kFaceModelNotReadyMessage          = "Face model is not ready";
		constexpr auto kFaceDetectorInitializationMessage = "Failed to initialize face detector";
		constexpr auto kFaceRecognizerInitializationMessage =
		    "Failed to initialize face recognizer";
		constexpr auto kFaceEncodingProcessingFailureMessage =
		    "Face encoding failed while processing camera frame";

		// Allocation failure (for example RLIMIT_AS exhaustion in the compare sandbox) is the one
		// cause worth naming; other OpenCV text stays out of the stable, sanitized diagnostic.
		auto IsOutOfMemory(const std::exception &error) -> bool {
			if (dynamic_cast<const std::bad_alloc *>(&error) != nullptr) {
				return true;
			}
			const auto *cv_error = dynamic_cast<const cv::Exception *>(&error);
			return cv_error != nullptr && cv_error->code == cv::Error::StsNoMem;
		}

		auto InitializationFailureMessage(const char *base, const std::exception &error)
		    -> std::string {
			return IsOutOfMemory(error) ? std::string(base) + ": out of memory" : std::string(base);
		}

		void AlignFace(void *context, const FaceAlignmentRequest &request) {
			static_cast<cv::FaceRecognizerSF *>(context)->alignCrop(
			    request.frame.get(), request.face.get(), request.aligned.get());
		}

		void ExtractFeature(void *context, const cv::Mat &aligned, cv::Mat &feature) {
			static_cast<cv::FaceRecognizerSF *>(context)->feature(aligned, feature);
		}

		// Per-version mapping is documented at ForcedDnnEngine() in vision/face_model/internal.hpp.
#if CV_VERSION_MAJOR == 5 && CV_VERSION_MINOR == 0
		constexpr int kForcedEngine = cv::dnn::ENGINE_NEW;
#else
		constexpr int kForcedEngine = cv::dnn::ENGINE_OPENCV;
#endif
		static_assert(kForcedEngine > 0 && kForcedEngine <= 9,
		              "OPENCV_FORCE_DNN_ENGINE value must be a single digit");
		constexpr std::array<char, 2> kForcedEngineValue{
		    static_cast<char>('0' + kForcedEngine),
		    '\0',
		};
	}  // namespace

	auto ForcedDnnEngine() -> const char * {
		return kForcedEngineValue.data();
	}

	auto ConfigureFaceModelEnvironment() -> bool {
		// Avoid environment mutation when compare has configured it before its worker starts.
		const auto *engine = getenv("OPENCV_FORCE_DNN_ENGINE");
		if (engine != nullptr && std::string_view(engine) == ForcedDnnEngine()) {
			return true;
		}
		// Pin the intended engine: OpenCV 5.0 would otherwise fall back to the Classic engine,
		// and an inherited value could select another engine such as ONNX Runtime.
		return setenv("OPENCV_FORCE_DNN_ENGINE", ForcedDnnEngine(), 1) == 0;
	}

	FaceModel::FaceModel(const FaceConfig &config)
	    : metric_(config.sface_metric)
	    , backend_(std::make_unique<Backend>()) {
		backend_->check_readiness = [](const std::filesystem::path &path) -> OpenCvModelReadiness {
			return CheckOpencvModelReadinessWithLabel(path, "OpenCV face model file",
			                                          static_cast<uid_t>(0));
		};
		backend_->create_detector = [](const std::string &path, const cv::Size &size,
		                               float score_threshold, float nms_threshold,
		                               int top_k) -> cv::Ptr<cv::FaceDetectorYN> {
			return cv::FaceDetectorYN::create(path, "", size, score_threshold, nms_threshold,
			                                  top_k);
		};
		backend_->create_recognizer = [](const std::string &path) -> cv::Ptr<cv::FaceRecognizerSF> {
			return cv::FaceRecognizerSF::create(path, "");
		};
		Initialize(config);
		if (ok_) {
			const auto detector      = detector_;
			backend_->set_input_size = [detector](const cv::Size &size) -> void {
				detector->setInputSize(size);
			};
			backend_->detect = [detector](const cv::Mat &frame, cv::Mat &faces) -> void {
				detector->detect(frame, faces);
			};
		}
	}

	FaceModel::~FaceModel()                                         = default;
	FaceModel::FaceModel(FaceModel &&) noexcept                     = default;
	auto FaceModel::operator=(FaceModel &&) noexcept -> FaceModel & = default;

	FaceModel::FaceModel(const FaceConfig &config, Backend backend)
	    : metric_(config.sface_metric)
	    , backend_(std::make_unique<Backend>(std::move(backend))) {
		Initialize(config);
	}

	void FaceModel::Initialize(const FaceConfig &config) {
		const auto models_dir      = ResolveModelsDir();
		const auto yunet_model     = (models_dir / kYunetModel).string();
		const auto sface_model     = (models_dir / kSfaceModel).string();
		const auto score_threshold = config.yunet_score_threshold;
		const auto nms_threshold   = config.yunet_nms_threshold;
		const auto top_k           = config.yunet_top_k;

		if (!backend_->check_readiness) {
			SetError(kFaceModelNotReadyMessage);
			return;
		}

		for (const auto &model_path : {yunet_model, sface_model}) {
			OpenCvModelReadiness readiness;
			try {
				readiness = backend_->check_readiness(model_path);
			} catch (const std::exception &) {
				SetError(kFaceModelNotReadyMessage);
				return;
			}
			if (readiness.status != OpenCvModelStatus::kOk) {
				SetError(kFaceModelNotReadyMessage);
				return;
			}
		}

		if (!ConfigureFaceModelEnvironment()) {
			SetError("OpenCV New DNN graph engine could not be configured");
			return;
		}

		if (!backend_->create_detector) {
			SetError(kFaceDetectorInitializationMessage);
			return;
		}
		try {
			detector_ = backend_->create_detector(yunet_model, input_size_, score_threshold,
			                                      nms_threshold, top_k);
		} catch (const std::exception &error) {
			SetError(InitializationFailureMessage(kFaceDetectorInitializationMessage, error));
			return;
		}
		if (detector_.empty()) {
			SetError(kFaceDetectorInitializationMessage);
			return;
		}

		if (!backend_->create_recognizer) {
			SetError(kFaceRecognizerInitializationMessage);
			return;
		}
		try {
			recognizer_ = backend_->create_recognizer(sface_model);
		} catch (const std::exception &error) {
			SetError(InitializationFailureMessage(kFaceRecognizerInitializationMessage, error));
			return;
		}
		if (recognizer_.empty()) {
			SetError(kFaceRecognizerInitializationMessage);
			return;
		}

		ok_ = true;
	}

	auto FaceModel::Ok() const -> bool {
		return ok_;
	}

	auto FaceModel::ErrorMessage() const -> const std::string & {
		return error_message_;
	}

	auto FaceModel::Metric() const -> FaceMetric {
		return metric_;
	}

	auto FaceModel::PrepareFrame(const cv::Mat &frame) -> cv::Mat {
		if (frame.channels() == 1) {
			cv::Mat converted;
			cv::cvtColor(frame, converted, cv::COLOR_GRAY2BGR);
			return converted;
		}
		if (frame.channels() == 4) {
			cv::Mat converted;
			cv::cvtColor(frame, converted, cv::COLOR_BGRA2BGR);
			return converted;
		}
		return frame;
	}

	auto FaceModel::Detect(const cv::Mat &frame) -> FaceDetectionResult {
		if (!ok_) {
			return {
			    .status        = FaceDetectionStatus::kInferenceError,
			    .error_message = kFaceModelNotReadyMessage,
			};
		}
		if (ValidateFrame(frame, FrameChannelPolicy::kCameraInput) !=
		    FrameValidationStatus::kValid) {
			return {
			    .status        = FaceDetectionStatus::kInferenceError,
			    .error_message = "Face detection failed: invalid input frame",
			};
		}
		if (backend_ == nullptr || !backend_->detect) {
			return {
			    .status        = FaceDetectionStatus::kInferenceError,
			    .error_message = "Internal error: missing face detection backend callback",
			};
		}

		try {
			cv::Mat const prepared = PrepareFrame(frame);
			if (cv::Size(prepared.cols, prepared.rows) != input_size_ &&
			    !backend_->set_input_size) {
				return {
				    .status        = FaceDetectionStatus::kInferenceError,
				    .error_message = "Internal error: missing face input-size backend callback",
				};
			}
			SetInputSizeFromFrame(prepared);

			cv::Mat faces;
			backend_->detect(prepared, faces);
			return ParseYunetDetections(faces);
		} catch (const cv::Exception &) {
			return FaceDetectionResult{
			    .status        = FaceDetectionStatus::kInferenceError,
			    .error_message = kFaceDetectionFailedMessage,
			};
		} catch (const std::exception &) {
			return FaceDetectionResult{
			    .status        = FaceDetectionStatus::kInferenceError,
			    .error_message = kFaceDetectionFailedMessage,
			};
		}
	}

	auto FaceModel::Encode(const cv::Mat &frame, const FaceDetection &face) -> FaceEncodingResult {
		if (!ok_) {
			return {
			    .status        = FaceEncodingStatus::kInferenceError,
			    .error_message = kFaceModelNotReadyMessage,
			};
		}
		if (ValidateFrame(frame, FrameChannelPolicy::kCameraInput) !=
		    FrameValidationStatus::kValid) {
			return {
			    .status        = FaceEncodingStatus::kInferenceError,
			    .error_message = "Face encoding failed: invalid input frame",
			};
		}
		if (recognizer_.empty()) {
			return {
			    .status        = FaceEncodingStatus::kInferenceError,
			    .error_message = kMissingSfaceEncodingDependencyMessage,
			};
		}

		cv::Mat prepared;
		try {
			prepared = PrepareFrame(frame);
		} catch (const cv::Exception &) {
			return {
			    .status        = FaceEncodingStatus::kInferenceError,
			    .error_message = "Face encoding failed during frame preparation",
			};
		}

		try {
			return EncodeSface(prepared, face,
			                   {
			                       .context         = recognizer_.get(),
			                       .align_face      = AlignFace,
			                       .extract_feature = ExtractFeature,
			                   });
		} catch (const cv::Exception &) {
			return {
			    .status        = FaceEncodingStatus::kInferenceError,
			    .error_message = kFaceEncodingProcessingFailureMessage,
			};
		} catch (const std::exception &) {
			return {
			    .status        = FaceEncodingStatus::kInferenceError,
			    .error_message = kFaceEncodingProcessingFailureMessage,
			};
		}
	}

	auto FaceModel::WarmUpRecognizer() -> bool {
		if (!ok_ || recognizer_warm_up_attempted_) {
			return false;
		}
		recognizer_warm_up_attempted_ = true;
		try {
			const cv::Mat       frame(112, 112, CV_8UC3, cv::Scalar(127, 127, 127));
			const FaceDetection face{
			    .box = {0.0F, 0.0F, 112.0F, 112.0F},
			    .landmarks =
			        {
			            {
			                {38.2946F, 51.6963F},
			                {73.5318F, 51.5014F},
			                {56.0252F, 71.7366F},
			                {41.5493F, 92.3655F},
			                {70.7299F, 92.2041F},
			            },
			        },
			    .confidence = 1.0F,
			};
			return FaceEncodingSucceeded(Encode(frame, face));
		} catch (...) {
			return false;
		}
	}

	void FaceModel::SetInputSizeFromFrame(const cv::Mat &frame) {
		const cv::Size new_size(frame.cols, frame.rows);
		if (new_size == input_size_) {
			return;
		}

		backend_->set_input_size(new_size);
		input_size_ = new_size;
	}

	void FaceModel::SetError(std::string message) {
		error_message_ = std::move(message);
		ok_            = false;
	}

	namespace {
		auto FaceModelPrepareFrameAdapter([[maybe_unused]] void *context, const cv::Mat &frame)
		    -> cv::Mat {
			return FaceModel::PrepareFrame(frame);
		}

		auto FaceModelDetectFacesAdapter(void *context, const cv::Mat &frame)
		    -> FaceDetectionResult {
			return static_cast<FaceModel *>(context)->Detect(frame);
		}

		auto FaceModelEncodeFaceAdapter(void *context, const cv::Mat &frame,
		                                const FaceDetection &face) -> FaceEncodingResult {
			return static_cast<FaceModel *>(context)->Encode(frame, face);
		}

	}  // namespace

	auto FaceModelInferenceOperations(FaceModel &model) -> FaceInferenceOperations {
		return {
		    .context       = &model,
		    .prepare_frame = FaceModelPrepareFrameAdapter,
		    .detect_faces  = FaceModelDetectFacesAdapter,
		    .encode_face   = FaceModelEncodeFaceAdapter,
		};
	}

}  // namespace howdy::native
