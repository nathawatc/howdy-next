#include "vision/face_model.hpp"

#include "config/runtime_config.hpp"
#include "config/runtime_paths.hpp"
#include "model_assets/model_file.hpp"
#include "vision/face_encoding/internal.hpp"
#include "vision/face_model/internal.hpp"
#include "vision/frame_validation.hpp"

#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace howdy::native {
	namespace {
		constexpr auto kFaceModelNotReadyMessage          = "Face model is not ready";
		constexpr auto kFaceDetectorInitializationMessage = "Failed to initialize face detector";
		constexpr auto kFaceRecognizerInitializationMessage =
		    "Failed to initialize face recognizer";
		constexpr auto kFaceEncodingProcessingFailureMessage =
		    "Face encoding failed while processing camera frame";

		// Temporary OpenCV 5 workaround: force New DNN graph engine and forbid
		// Classic-engine fallback, which cannot load Howdy's supported ONNX models.
		// Remove after upstream makes New engine selection/failure behavior suitable.
		auto ForceOpencvNewDnnEngine() -> bool {
			return setenv("OPENCV_FORCE_DNN_ENGINE", "2", 1) == 0;
		}

		void AlignFace(void *context, const FaceAlignmentRequest &request) {
			static_cast<cv::FaceRecognizerSF *>(context)->alignCrop(request.frame, request.face,
			                                                        request.aligned);
		}

		void ExtractFeature(void *context, const cv::Mat &aligned, cv::Mat &feature) {
			static_cast<cv::FaceRecognizerSF *>(context)->feature(aligned, feature);
		}
	}  // namespace

	FaceModel::FaceModel(const FaceConfig &config)
	    : metric_(config.sface_metric)
	    , threshold_(config.sface_threshold)
	    , backend_(std::make_shared<Backend>()) {
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

	FaceModel::FaceModel(const FaceConfig &config, Backend backend)
	    : metric_(config.sface_metric)
	    , threshold_(config.sface_threshold)
	    , backend_(std::make_shared<Backend>(std::move(backend))) {
		Initialize(config);
	}

	void FaceModel::Initialize(const FaceConfig &config) {
		const auto models_dir      = ResolveModelsDir();
		const auto yunet_model     = (models_dir / kYunetModel).string();
		const auto sface_model     = (models_dir / kSfaceModel).string();
		const auto score_threshold = config.yunet_score_threshold;
		const auto nms_threshold   = config.yunet_nms_threshold;
		const auto top_k           = config.yunet_top_k;

		if (backend_ == nullptr || !backend_->check_readiness) {
			SetError(FaceModelErrorCategory::kModelNotReady, kFaceModelNotReadyMessage);
			return;
		}

		for (const auto &model_path : {yunet_model, sface_model}) {
			OpenCvModelReadiness readiness;
			try {
				readiness = backend_->check_readiness(model_path);
			} catch (const std::exception &) {
				SetError(FaceModelErrorCategory::kModelNotReady, kFaceModelNotReadyMessage);
				return;
			}
			if (readiness.status != OpenCvModelStatus::kOk) {
				SetError(FaceModelErrorCategory::kModelNotReady, kFaceModelNotReadyMessage);
				return;
			}
		}

		if (!ForceOpencvNewDnnEngine()) {
			SetError(FaceModelErrorCategory::kDetectorInitialization,
			         "OpenCV New DNN graph engine could not be configured");
			return;
		}

		if (!backend_->create_detector) {
			SetError(FaceModelErrorCategory::kDetectorInitialization,
			         kFaceDetectorInitializationMessage);
			return;
		}
		try {
			detector_ = backend_->create_detector(yunet_model, input_size_, score_threshold,
			                                      nms_threshold, top_k);
		} catch (const cv::Exception &) {
			SetError(FaceModelErrorCategory::kDetectorInitialization,
			         kFaceDetectorInitializationMessage);
			return;
		} catch (const std::exception &) {
			SetError(FaceModelErrorCategory::kDetectorInitialization,
			         kFaceDetectorInitializationMessage);
			return;
		}
		if (detector_.empty()) {
			SetError(FaceModelErrorCategory::kDetectorInitialization,
			         kFaceDetectorInitializationMessage);
			return;
		}

		if (!backend_->create_recognizer) {
			SetError(FaceModelErrorCategory::kRecognizerInitialization,
			         kFaceRecognizerInitializationMessage);
			return;
		}
		try {
			recognizer_ = backend_->create_recognizer(sface_model);
		} catch (const cv::Exception &) {
			SetError(FaceModelErrorCategory::kRecognizerInitialization,
			         kFaceRecognizerInitializationMessage);
			return;
		} catch (const std::exception &) {
			SetError(FaceModelErrorCategory::kRecognizerInitialization,
			         kFaceRecognizerInitializationMessage);
			return;
		}
		if (recognizer_.empty()) {
			SetError(FaceModelErrorCategory::kRecognizerInitialization,
			         kFaceRecognizerInitializationMessage);
			return;
		}

		ok_ = true;
	}

	auto FaceModel::Ok() const -> bool {
		return ok_;
	}

	auto FaceModel::ErrorCategory() const -> FaceModelErrorCategory {
		return error_category_;
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
			cv::Mat prepared = PrepareFrame(frame);
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

	auto FaceModel::BestMatch(const std::vector<std::vector<float>> &known,
	                          const std::vector<float>              &probe) const -> FaceMatch {
		return FindBestFaceMatch(known, probe, metric_, threshold_);
	}

	void FaceModel::SetInputSizeFromFrame(const cv::Mat &frame) {
		const cv::Size new_size(frame.cols, frame.rows);
		if (new_size == input_size_) {
			return;
		}

		backend_->set_input_size(new_size);
		input_size_ = new_size;
	}

	void FaceModel::SetError(FaceModelErrorCategory category, std::string message) {
		error_category_ = category;
		error_message_  = std::move(message);
		ok_             = false;
	}

}  // namespace howdy::native
