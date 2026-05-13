#include "core/face_model.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <utility>

#include <opencv2/imgproc.hpp>

#include "config/runtime_paths.hpp"

namespace howdy::native {

FaceModel::FaceModel(const ConfigReader &config) {
  const auto models_dir = resolve_models_dir();
  const auto yunet_model = resolve_model_path(
      config, "yunet_model", (models_dir / kYunetModel).string());
  const auto sface_model = resolve_model_path(
      config, "sface_model", (models_dir / kSfaceModel).string());

  for (const auto &model_path : {yunet_model, sface_model}) {
    if (!std::filesystem::is_regular_file(model_path)) {
      set_error("OpenCV face model file is missing: " + model_path);
      return;
    }
    if (bad_model_download(model_path)) {
      set_error("OpenCV face model file is invalid: " + model_path);
      return;
    }
  }

  const auto score_threshold =
      config.get_float("face", "yunet_score_threshold", 0.9F);
  const auto nms_threshold =
      config.get_float("face", "yunet_nms_threshold", 0.3F);
  const auto top_k = config.get_int("face", "yunet_top_k", 5000);
  metric_ = config.get("face", "sface_metric", "cosine");
  std::transform(metric_.begin(), metric_.end(), metric_.begin(), ::tolower);
  threshold_ =
      config.get_float("face", "sface_threshold",
                       metric_ == "cosine" ? 0.363F : 1.128F);

  try {
    detector_ = cv::FaceDetectorYN::create(yunet_model, "", input_size_,
                                           score_threshold, nms_threshold, top_k);
    recognizer_ = cv::FaceRecognizerSF::create(sface_model, "");
  } catch (const cv::Exception &error) {
    set_error(error.what());
    return;
  }

  ok_ = true;
}

auto FaceModel::ok() const -> bool { return ok_; }

auto FaceModel::error_message() const -> const std::string & {
  return error_message_;
}

auto FaceModel::metric() const -> const std::string & { return metric_; }

auto FaceModel::threshold() const -> float { return threshold_; }

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
  cv::Mat prepared = prepare_frame(frame);
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

auto FaceModel::encode(const cv::Mat &frame, const cv::Mat &face)
    -> std::vector<float> {
  std::vector<float> result;
  cv::Mat prepared = prepare_frame(frame);
  cv::Mat aligned;
  cv::Mat feature;

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
                           const std::vector<float> &probe) const -> FaceMatch {
  FaceMatch match;
  if (known.empty() || probe.empty()) {
    match.score = metric_ == "cosine" ? -1.0F : std::numeric_limits<float>::max();
    return match;
  }

  if (metric_ == "cosine") {
    float best_score = -1.0F;
    int best_index = -1;
    float probe_norm = 0.0F;
    for (float value : probe) {
      probe_norm += value * value;
    }
    probe_norm = std::sqrt(std::max(probe_norm, 1.0e-12F));

    for (std::size_t index = 0; index < known.size(); ++index) {
      const auto &candidate = known[index];
      if (candidate.size() != probe.size()) {
        continue;
      }

      float dot = 0.0F;
      float known_norm = 0.0F;
      for (std::size_t element = 0; element < probe.size(); ++element) {
        dot += candidate[element] * probe[element];
        known_norm += candidate[element] * candidate[element];
      }

      known_norm = std::sqrt(std::max(known_norm, 1.0e-12F));
      const float score = dot / std::max(known_norm * probe_norm, 1.0e-12F);
      if (score > best_score) {
        best_score = score;
        best_index = static_cast<int>(index);
      }
    }

    match.index = best_index;
    match.score = best_score;
    match.accepted = best_index >= 0 && best_score >= threshold_;
    return match;
  }

  float best_score = std::numeric_limits<float>::max();
  int best_index = -1;
  for (std::size_t index = 0; index < known.size(); ++index) {
    const auto &candidate = known[index];
    if (candidate.size() != probe.size()) {
      continue;
    }

    float sum = 0.0F;
    for (std::size_t element = 0; element < probe.size(); ++element) {
      const float delta = candidate[element] - probe[element];
      sum += delta * delta;
    }

    const float score = std::sqrt(sum);
    if (score < best_score) {
      best_score = score;
      best_index = static_cast<int>(index);
    }
  }

  match.index = best_index;
  match.score = best_score;
  match.accepted = best_index >= 0 && best_score > 0.0F && best_score <= threshold_;
  return match;
}

auto FaceModel::detection_box(const cv::Mat &face) const
    -> std::tuple<int, int, int, int> {
  return {static_cast<int>(face.at<float>(0, 0)),
          static_cast<int>(face.at<float>(0, 1)),
          static_cast<int>(face.at<float>(0, 2)),
          static_cast<int>(face.at<float>(0, 3))};
}

auto FaceModel::detection_landmarks(const cv::Mat &face) const
    -> std::vector<cv::Point> {
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
  ok_ = false;
}

auto FaceModel::resolve_model_path(const ConfigReader &config,
                                   const std::string &option,
                                   const std::string &fallback) const
    -> std::string {
  std::string value = config.get("face", option, fallback);
  if (value.empty() || value == "default" || value == "none") {
    return fallback;
  }
  return value;
}

auto FaceModel::bad_model_download(const std::string &path) const -> bool {
  std::ifstream model_file(path, std::ios::binary);
  if (!model_file.is_open()) {
    return true;
  }

  std::string header(256, '\0');
  model_file.read(header.data(), static_cast<std::streamsize>(header.size()));
  header.resize(static_cast<std::size_t>(model_file.gcount()));
  return header.rfind("version https://git-lfs.github.com/spec/v1", 0) == 0 ||
         header.find('<') == 0;
}

}  // namespace howdy::native
