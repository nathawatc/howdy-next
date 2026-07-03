#include "core/face_model.hpp"

#include "common/model_file.hpp"
#include "config/runtime_paths.hpp"
#include "core/face_encoding_internal.hpp"

#include <filesystem>
#include <string>
#include <utility>

#include <opencv2/imgproc.hpp>

namespace howdy::native {
	namespace {
		void align_face(void *context, const cv::Mat &frame, const cv::Mat &face,
		                cv::Mat &aligned) {
			static_cast<cv::FaceRecognizerSF *>(context)->alignCrop(frame, face, aligned);
		}

		void extract_feature(void *context, const cv::Mat &aligned, cv::Mat &feature) {
			static_cast<cv::FaceRecognizerSF *>(context)->feature(aligned, feature);
		}
	}  // namespace

	FaceModel::FaceModel(const FaceConfig &config)
	    : metric_(config.sface_metric)
	    , threshold_(config.sface_threshold) {
		const auto models_dir = resolve_models_dir();
		const auto yunet_model =
		    resolve_model_path(config.yunet_model, (models_dir / kYunetModel).string());
		const auto sface_model =
		    resolve_model_path(config.sface_model, (models_dir / kSfaceModel).string());

		for (const auto &model_path : {yunet_model, sface_model}) {
			const auto readiness =
			    check_opencv_face_model_readiness(model_path, static_cast<uid_t>(0));
			if (readiness.status != OpenCvModelStatus::kOk) {
				set_error(readiness.error_message);
				return;
			}
		}

		const auto score_threshold = config.yunet_score_threshold;
		const auto nms_threshold   = config.yunet_nms_threshold;
		const auto top_k           = config.yunet_top_k;

		try {
			detector_   = cv::FaceDetectorYN::create(yunet_model, "", input_size_, score_threshold,
			                                         nms_threshold, top_k);
			recognizer_ = cv::FaceRecognizerSF::create(sface_model, "");
		} catch (const cv::Exception &error) {
			set_error(error.what());
			return;
		}

		ok_ = true;
	}

	auto FaceModel::ok() const -> bool {
		return ok_;
	}

	auto FaceModel::error_message() const -> const std::string & {
		return error_message_;
	}

	auto FaceModel::metric() const -> const std::string & {
		return metric_;
	}

	auto FaceModel::prepare_frame(const cv::Mat &frame) const -> cv::Mat {
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

	auto FaceModel::detect(const cv::Mat &frame) -> FaceDetectionResult {
		try {
			cv::Mat prepared = prepare_frame(frame);
			set_input_size_from_frame(prepared);

			cv::Mat faces;
			detector_->detect(prepared, faces);
			return parse_yunet_detections(faces);
		} catch (const cv::Exception &error) {
			return FaceDetectionResult{
			    .status        = FaceDetectionStatus::kInferenceError,
			    .error_message = std::string("YuNet inference failed: ") + error.what(),
			};
		}
	}

	auto FaceModel::encode(const cv::Mat &frame, const FaceDetection &face) -> FaceEncodingResult {
		cv::Mat prepared;
		try {
			prepared = prepare_frame(frame);
		} catch (const cv::Exception &) {
			return {
			    .status        = FaceEncodingStatus::kInferenceError,
			    .error_message = "Face encoding failed during frame preparation",
			};
		}

		try {
			return encode_sface(prepared, face,
			                    {
			                        .context         = recognizer_.get(),
			                        .align_face      = align_face,
			                        .extract_feature = extract_feature,
			                    });
		} catch (const cv::Exception &) {
			return {
			    .status        = FaceEncodingStatus::kInferenceError,
			    .error_message = "Face encoding failed while processing camera frame",
			};
		}
	}

	auto FaceModel::best_match(const std::vector<std::vector<float>> &known,
	                           const std::vector<float>              &probe) const -> FaceMatch {
		return find_best_face_match(known, probe, metric_, threshold_);
	}

	void FaceModel::set_input_size_from_frame(const cv::Mat &frame) {
		const cv::Size new_size(frame.cols, frame.rows);
		if (new_size == input_size_) {
			return;
		}

		detector_->setInputSize(new_size);
		input_size_ = new_size;
	}

	void FaceModel::set_error(std::string message) {
		error_message_ = std::move(message);
		ok_            = false;
	}

	auto FaceModel::resolve_model_path(const std::string &value, const std::string &fallback) const
	    -> std::string {
		if (value.empty() || value == "default" || value == "none") {
			return fallback;
		}
		return value;
	}

}  // namespace howdy::native
