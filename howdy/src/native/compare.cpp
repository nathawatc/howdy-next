#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

#include <opencv2/imgproc.hpp>

#include "howdy/common/compare_exit.hpp"
#include "howdy/config/config_reader.hpp"
#include "howdy/config/runtime_paths.hpp"
#include "howdy/core/face_model.hpp"
#include "howdy/recorders/video_capture.hpp"
#include "howdy/storage/user_models.hpp"

namespace {

using howdy::native::CompareExit;

struct CompareArgs {
  std::string user;
  std::string config_path = howdy::native::resolve_config_path().string();
};

auto parse_args(int argc, char *argv[]) -> CompareArgs {
  CompareArgs args;

  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: " << argv[0] << " [--config PATH] <user>\n";
      std::exit(static_cast<int>(CompareExit::kSuccess));
    }
    if (arg == "--config" && index + 1 < argc) {
      args.config_path = argv[++index];
      continue;
    }
    if (arg.starts_with('-')) {
      std::cerr << "Unknown argument: " << arg << "\n";
      std::exit(static_cast<int>(CompareExit::kAbort));
    }
    args.user = argv[index];
  }

  if (args.user.empty()) {
    std::exit(static_cast<int>(CompareExit::kAbort));
  }

  return args;
}

auto update_best_score(float current, float score, const std::string &metric)
    -> float {
  if (std::isnan(current)) {
    return score;
  }
  if (metric == "cosine") {
    return std::max(current, score);
  }
  return std::min(current, score);
}

auto apply_rotation(const cv::Mat &frame, int rotate, int frames) -> cv::Mat {
  if (rotate == 1) {
    if (frames % 3 == 1) {
      cv::Mat rotated;
      cv::rotate(frame, rotated, cv::ROTATE_90_COUNTERCLOCKWISE);
      return rotated;
    }
    if (frames % 3 == 2) {
      cv::Mat rotated;
      cv::rotate(frame, rotated, cv::ROTATE_90_CLOCKWISE);
      return rotated;
    }
  } else if (rotate == 2) {
    cv::Mat rotated;
    cv::rotate(frame, rotated,
               frames % 2 == 0 ? cv::ROTATE_90_COUNTERCLOCKWISE
                               : cv::ROTATE_90_CLOCKWISE);
    return rotated;
  }

  return frame;
}

}  // namespace

auto main(int argc, char *argv[]) -> int {
  const auto start_time = std::chrono::steady_clock::now();
  const CompareArgs args = parse_args(argc, argv);

  const auto loaded_models =
      howdy::native::load_user_models(args.user, howdy::native::FaceModel::kBackendName);
  if (loaded_models.status == howdy::native::UserModelStatus::kIncompatibleBackend) {
    std::cerr << loaded_models.error_message << "\n";
    return static_cast<int>(CompareExit::kNoFaceModel);
  }
  if (loaded_models.status == howdy::native::UserModelStatus::kParseError) {
    std::cerr << loaded_models.error_message << "\n";
    return static_cast<int>(CompareExit::kAbort);
  }
  if (loaded_models.status != howdy::native::UserModelStatus::kOk) {
    return static_cast<int>(CompareExit::kNoFaceModel);
  }

  howdy::native::ConfigReader config(args.config_path);
  if (!config.ok()) {
    std::cerr << "Failed to parse config: " << args.config_path << "\n";
    return static_cast<int>(CompareExit::kAbort);
  }

  howdy::native::FaceModel face_model(config);
  if (!face_model.ok()) {
    std::cerr << face_model.error_message() << "\n";
    return static_cast<int>(CompareExit::kAbort);
  }

  howdy::native::VideoCapture capture(howdy::native::load_capture_settings(config));
  if (!capture.open()) {
    std::cerr << capture.error_message() << "\n";
    return static_cast<int>(CompareExit::kInvalidDevice);
  }

  const int timeout = config.get_int("video", "timeout", 4);
  const float dark_threshold = config.get_float("video", "dark_threshold", 50.0F);
  const float max_height = config.get_float("video", "max_height", 320.0F);
  const int rotate = config.get_int("video", "rotate", 0);
  const int exposure = config.get_int("video", "exposure", -1);
  const bool end_report = config.get_bool("debug", "end_report", false);
  const bool use_clahe = config.get_bool("video", "clahe_enabled", true);
  const double clip_limit = config.get_float("video", "clahe_clip_limit", 1.25F);
  const int tile_size = config.get_int("video", "clahe_tile_grid_size", 8);

  auto native_height = capture.get(cv::CAP_PROP_FRAME_HEIGHT);
  if (rotate == 2) {
    native_height = capture.get(cv::CAP_PROP_FRAME_WIDTH);
  }
  const double scaling_factor = max_height / std::max(native_height, 1.0);

  cv::Ptr<cv::CLAHE> clahe;
  if (use_clahe) {
    clahe = cv::createCLAHE(clip_limit, cv::Size(tile_size, tile_size));
  }

  int frames = 0;
  int black_tries = 0;
  int dark_tries = 0;
  int valid_frames = 0;
  double dark_running_total = 0.0;
  float best_score = std::numeric_limits<float>::quiet_NaN();
  float winning_score = 0.0F;
  int winning_index = -1;
  auto frame_loop_start = std::chrono::steady_clock::now();

  while (true) {
    frames++;

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - frame_loop_start)
            .count();
    if (elapsed > timeout) {
      if (dark_tries > 0 && valid_frames == dark_tries) {
        std::cerr << "All frames were too dark, please check dark_threshold in config\n";
        std::cerr << "Average darkness: "
                  << (dark_running_total / std::max(valid_frames, 1))
                  << ", Threshold: " << dark_threshold << "\n";
        return static_cast<int>(CompareExit::kTooDark);
      }
      return static_cast<int>(CompareExit::kTimeoutReached);
    }

    cv::Mat frame;
    cv::Mat gray_frame;
    if (!capture.read(frame, &gray_frame)) {
      std::cerr << capture.error_message() << "\n";
      return static_cast<int>(CompareExit::kInvalidDevice);
    }

    if (use_clahe) {
      clahe->apply(gray_frame, gray_frame);
    }

    cv::Mat hist;
    constexpr int hist_size[] = {8};
    constexpr float hist_range[] = {0.0F, 256.0F};
    const float *ranges[] = {hist_range};
    constexpr int channels[] = {0};
    cv::calcHist(&gray_frame, 1, channels, cv::Mat(), hist, 1, hist_size, ranges);
    const double hist_total = cv::sum(hist)[0];
    if (hist_total == 0.0) {
      black_tries++;
      continue;
    }

    const float darkness =
        static_cast<float>(hist.at<float>(0) / hist_total * 100.0);
    if (darkness == 100.0F) {
      black_tries++;
      continue;
    }

    dark_running_total += darkness;
    valid_frames++;

    if (darkness > dark_threshold) {
      dark_tries++;
      continue;
    }

    cv::Mat working_frame = gray_frame;
    if (scaling_factor != 1.0) {
      cv::resize(gray_frame, working_frame, cv::Size(), scaling_factor,
                 scaling_factor, cv::INTER_AREA);
    }
    working_frame = apply_rotation(working_frame, rotate, frames);

    cv::Mat prepared = face_model.prepare_frame(working_frame);
    const auto faces = face_model.detect(prepared);
    for (const auto &face : faces) {
      const auto encoding = face_model.encode(prepared, face);
      const auto match = face_model.best_match(loaded_models.stored.encodings, encoding);
      best_score = update_best_score(best_score, match.score, face_model.metric());

      if (!match.accepted) {
        continue;
      }

      winning_index = match.index;
      winning_score = match.score;

      if (end_report) {
        const auto total_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time)
                .count();
        std::cout << "Total time: " << total_ms << "ms\n";
        std::cout << "Frames searched: " << frames << "\n";
        std::cout << "Black frames ignored: " << black_tries << "\n";
        std::cout << "Dark frames ignored: " << dark_tries << "\n";
        std::cout << "Winning score: " << winning_score << "\n";
        if (winning_index >= 0 &&
            winning_index < static_cast<int>(loaded_models.stored.models.size())) {
          const auto &winner =
              loaded_models.stored.models[static_cast<std::size_t>(winning_index)];
          std::cout << "Winning model: " << winner.id << " (\"" << winner.label
                    << "\")\n";
        }
      }

      return static_cast<int>(CompareExit::kSuccess);
    }

    if (exposure != -1) {
      capture.set(cv::CAP_PROP_AUTO_EXPOSURE, 1.0);
      capture.set(cv::CAP_PROP_EXPOSURE, static_cast<double>(exposure));
    }
  }
}
