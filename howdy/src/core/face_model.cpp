#include "core/face_model.hpp"

#include "common/model_file.hpp"
#include "config/runtime_paths.hpp"

#include <filesystem>
#include <string>
#include <utility>

#include <opencv2/imgproc.hpp>

namespace howdy::native {

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

	auto FaceModel::detect(const cv::Mat &frame) -> std::vector<cv::Mat> {
		std::vector<cv::Mat> result;
		cv::Mat              prepared = prepare_frame(frame);
		set_input_size_from_frame(prepared);

		cv::Mat faces;
		detector_->detect(prepared, faces);
		if (faces.empty()) {
			return result;
		}

		result.reserve(static_cast<std::size_t>(faces.rows));
		for (int row = 0; row < faces.rows; ++row) {
			result.push_back(faces.row(row).clone());
		}
		return result;
	}

	auto FaceModel::encode(const cv::Mat &frame, const cv::Mat &face) -> std::vector<float> {
		std::vector<float> result;
		cv::Mat            prepared = prepare_frame(frame);
		cv::Mat            aligned;
		cv::Mat            feature;

		recognizer_->alignCrop(prepared, face, aligned);
		if (aligned.empty()) {
			return result;
		}

		recognizer_->feature(aligned, feature);
		if (feature.empty()) {
			return result;
		}

		const cv::Mat flattened = feature.reshape(1, 1);
		result.reserve(flattened.cols);
		for (int index = 0; index < flattened.cols; ++index) {
			result.push_back(flattened.at<float>(0, index));
		}
		return result;
	}

	auto FaceModel::best_match(const std::vector<std::vector<float>> &known,
	                           const std::vector<float>              &probe) const -> FaceMatch {
		return find_best_face_match(known, probe, metric_, threshold_);
	}

	auto FaceModel::detection_box(const cv::Mat &face) const -> std::tuple<int, int, int, int> {
		return {static_cast<int>(face.at<float>(0, 0)), static_cast<int>(face.at<float>(0, 1)),
		        static_cast<int>(face.at<float>(0, 2)), static_cast<int>(face.at<float>(0, 3))};
	}

	auto FaceModel::detection_landmarks(const cv::Mat &face) const -> std::vector<cv::Point> {
		std::vector<cv::Point> points;
		points.reserve(5);
		for (int index = 4; index < 14; index += 2) {
			points.emplace_back(static_cast<int>(face.at<float>(0, index)),
			                    static_cast<int>(face.at<float>(0, index + 1)));
		}
		return points;
	}

	auto FaceModel::detection_confidence(const cv::Mat &face) const -> float {
		return face.cols > 14 ? face.at<float>(0, 14) : 0.0F;
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
