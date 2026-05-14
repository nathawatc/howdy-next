#include "cli/add_cli.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <unistd.h>

#include <nlohmann/json.hpp>
#include <opencv2/imgproc.hpp>

#include "common/atomic_files.hpp"
#include "common/file_security.hpp"
#include "common/file_lock.hpp"
#include "common/user_names.hpp"
#include "config/config_reader.hpp"
#include "config/config_values.hpp"
#include "config/runtime_paths.hpp"
#include "core/face_model.hpp"
#include "recorders/video_capture.hpp"

namespace {

constexpr auto kExitOk = 0;
constexpr auto kExitAbort = 1;
constexpr int kMaxFrames = 60;
constexpr std::uintmax_t kMaxModelFileBytes = 1024 * 1024;

struct AddArgs {
  std::string user;
  std::string label;
  bool plain = false;
  bool yes = false;
};

auto parse_args(int argc, char **argv) -> AddArgs {
  AddArgs args;
  if (argc < 2) {
    std::cerr << "Usage: howdy-add <user> [label] [--plain] [-y]\n";
    std::exit(kExitAbort);
  }

  args.user = argv[1];
  for (int index = 2; index < argc; ++index) {
    std::string_view arg(argv[index]);
    if (arg == "--plain") {
      args.plain = true;
      continue;
    }
    if (arg == "-y") {
      args.yes = true;
      continue;
    }
    if (args.label.empty()) {
      args.label = argv[index];
    }
  }
  return args;
}

auto load_models(const std::filesystem::path &path) -> nlohmann::json {
  if (!std::filesystem::is_regular_file(path)) {
    return nlohmann::json::array();
  }

  std::error_code size_ec;
  if (std::filesystem::file_size(path, size_ec) > kMaxModelFileBytes || size_ec) {
    return nlohmann::json::array();
  }

  std::ifstream input(path);
  if (!input.is_open()) {
    return nlohmann::json::array();
  }

  nlohmann::json models;
  try {
    input >> models;
  } catch (const nlohmann::json::exception &) {
    return nlohmann::json::array();
  }
  if (!models.is_array()) {
    return nlohmann::json::array();
  }
  return models;
}

auto save_models_atomic(const std::filesystem::path &path,
                        const nlohmann::json &models) -> bool {
  return howdy::native::write_atomic_file(path, models.dump());
}

auto is_backend_compatible(const nlohmann::json &models) -> bool {
  return std::all_of(
      models.begin(), models.end(), [](const auto &entry) {
        const auto backend = entry.value("backend", std::string());
        return backend.empty() ||
               backend == howdy::native::FaceModel::kBackendName;
      });
}

}  // namespace

auto add_main(int argc, char **argv) -> int {
  const auto args = parse_args(argc, argv);
  const auto config_path = howdy::native::resolve_config_path();
  const auto config_security =
      howdy::native::check_secure_root_owned_file(config_path, "Config file");
  if (!config_security.ok) {
    std::cerr << config_security.error_message << "\n";
    return kExitAbort;
  }
  howdy::native::ConfigReader config(config_path.string());
  if (!config.ok()) {
    std::cerr << "Failed to parse config: " << config_path << "\n";
    return kExitAbort;
  }

  howdy::native::FaceModel face_model(config);
  if (!face_model.ok()) {
    std::cerr << face_model.error_message() << "\n";
    return kExitAbort;
  }

  const auto user_models_dir = howdy::native::resolve_user_models_dir();
  if (std::filesystem::exists(user_models_dir)) {
    const auto dir_security = howdy::native::check_secure_root_owned_directory(
        user_models_dir, "User models directory");
    if (!dir_security.ok) {
      std::cerr << dir_security.error_message << "\n";
      return kExitAbort;
    }
  }
  const auto model_path = howdy::native::resolve_user_model_path(user_models_dir, args.user);
  if (!model_path) {
    std::cerr << howdy::native::kInvalidUserNameMessage << "\n";
    return kExitAbort;
  }
  if (std::filesystem::exists(*model_path)) {
    const auto model_security =
        howdy::native::check_secure_root_owned_file(*model_path, "User model file");
    if (!model_security.ok) {
      std::cerr << model_security.error_message << "\n";
      return kExitAbort;
    }
  }

  const auto model_lock = howdy::native::acquire_file_lock(*model_path);
  if (!model_lock.has_value()) {
    std::cerr << "Failed to lock model file\n";
    return kExitAbort;
  }

  auto models = load_models(*model_path);

  if (!is_backend_compatible(models)) {
    std::cerr << "Existing face models use an incompatible backend.\n";
    std::cerr << "Please run `howdy clear` and enroll again with `howdy add`.\n";
    return kExitAbort;
  }

  int next_id = 0;
  if (!models.empty()) {
    next_id = models.back().value("id", -1) + 1;
  }

  std::string label = args.label.empty() ? ("Model #" + std::to_string(next_id))
                                         : args.label;
  if (!args.yes && args.label.empty() && !args.plain) {
    std::cout << "Enter a label for this new model [" << label << "]: ";
    std::string input;
    std::getline(std::cin, input);
    if (!input.empty()) {
      label = input.substr(0, 24);
    }
  }
  label.erase(std::remove(label.begin(), label.end(), ','), label.end());

  howdy::native::VideoCapture capture(howdy::native::load_capture_settings(config));
  if (!capture.open()) {
    std::cerr << capture.error_message() << "\n";
    return kExitAbort;
  }

  const float dark_threshold = howdy::native::config_dark_threshold(config);
  const bool use_clahe = config.get_bool("video", "clahe_enabled", true);
  const auto clip_limit = howdy::native::config_clahe_clip_limit(config);
  const auto tile_size = howdy::native::config_clahe_tile_grid_size(config);
  cv::Ptr<cv::CLAHE> clahe;
  if (use_clahe) {
    clahe = cv::createCLAHE(clip_limit, cv::Size(tile_size, tile_size));
  }

  if (!args.plain) {
    std::cout << "\nPlease look straight into the camera\n";
  }
  std::this_thread::sleep_for(std::chrono::seconds(2));

  cv::Mat frame;
  cv::Mat gray;
  std::vector<cv::Mat> faces;
  int valid_frames = 0;
  int dark_tries = 0;
  double dark_running_total = 0.0;

  for (int frame_count = 0; frame_count < kMaxFrames; ++frame_count) {
    if (!capture.read(frame, &gray)) {
      continue;
    }

    if (use_clahe) {
      clahe->apply(gray, gray);
    }

    cv::Mat hist;
    constexpr std::array<int, 1> hist_size{8};
    constexpr std::array<float, 2> hist_range{0.0F, 256.0F};
    std::vector<const float *> ranges{hist_range.data()};
    constexpr std::array<int, 1> channels{0};
    cv::calcHist(&gray, 1, channels.data(), cv::Mat(), hist, 1,
                 hist_size.data(), ranges.data());
    const double hist_total = cv::sum(hist)[0];
    if (hist_total == 0.0) {
      continue;
    }

    const auto darkness =
        static_cast<float>(hist.at<float>(0) / hist_total * 100.0);
    if (darkness >= 100.0F) {
      continue;
    }

    valid_frames++;
    dark_running_total += darkness;
    if (darkness > dark_threshold) {
      dark_tries++;
      continue;
    }

    auto prepared = face_model.prepare_frame(gray);
    faces = face_model.detect(prepared);
    if (!faces.empty()) {
      frame = prepared;
      break;
    }
  }

  capture.release();

  if (faces.empty()) {
    if (valid_frames == 0) {
      std::cerr << "Camera saw only black frames - is IR emitter working?\n";
    } else if (valid_frames == dark_tries) {
      std::cerr << "All frames were too dark, please check dark_threshold in config\n";
      std::cerr << "Average darkness: " << (dark_running_total / valid_frames)
                << ", Threshold: " << dark_threshold << "\n";
    } else {
      std::cerr << "No face detected, aborting\n";
    }
    return kExitAbort;
  }

  if (faces.size() > 1) {
    std::cerr << "Multiple faces detected, aborting\n";
    return kExitAbort;
  }

  auto encoding = face_model.encode(frame, faces.front());
  if (encoding.empty()) {
    std::cerr << "No valid face encoding captured\n";
    return kExitAbort;
  }

  nlohmann::json entry;
  entry["time"] = static_cast<long long>(std::time(nullptr));
  entry["label"] = label;
  entry["id"] = next_id;
  entry["backend"] = howdy::native::FaceModel::kBackendName;
  entry["metric"] = face_model.metric();
  entry["model"] = howdy::native::FaceModel::kSfaceModel;
  entry["data"] = nlohmann::json::array({encoding});
  models.push_back(entry);

  if (!save_models_atomic(*model_path, models)) {
    std::cerr << "Failed to save model file\n";
    return kExitAbort;
  }

  std::cout << "\nScan complete\nAdded a new model to " << args.user << "\n";
  return kExitOk;
}
