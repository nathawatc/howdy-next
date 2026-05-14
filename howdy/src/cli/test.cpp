#include "cli/test_cli.hpp"

#include "common/file_security.hpp"
#include "config/config_reader.hpp"
#include "config/config_values.hpp"
#include "config/runtime_paths.hpp"
#include "core/face_model.hpp"
#include "recorders/video_capture.hpp"
#include "storage/user_models.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <grp.h>
#include <iostream>
#include <limits>
#include <optional>
#include <pwd.h>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <unistd.h>

#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

namespace {

constexpr int kExitOk = 0;
constexpr int kExitCameraError = 1;
constexpr auto kWindowName = "Howdy Test";

struct InvokingUser {
  uid_t uid = 0;
  gid_t gid = 0;
  std::string name;
  std::string home;
  std::string shell;
};

struct TestArgs {
  std::string user;
  std::string device_path;
};

bool g_slow_mode = false;

void mouse_callback(int event, int x, int y, int flags, void *userdata) {
  (void)x;
  (void)y;
  (void)flags;
  (void)userdata;
  if (event == cv::EVENT_LBUTTONDOWN) {
    g_slow_mode = !g_slow_mode;
  }
}

auto parse_args(int argc, char **argv) -> TestArgs {
  TestArgs args;

  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "--device" && index + 1 < argc) {
      args.device_path = argv[++index];
      continue;
    }
    if (!arg.empty() && arg.front() != '-' && args.user.empty()) {
      args.user = argv[index];
    }
  }

  return args;
}

void print_text(cv::Mat &overlay, int line_number, int height,
                const std::string &text) {
  cv::putText(overlay, text, cv::Point(10, height - 10 - (10 * line_number)),
              cv::FONT_HERSHEY_SIMPLEX, 0.3, cv::Scalar(0, 255, 0), 0,
              cv::LINE_AA);
}

auto parse_uid_env(const char *value) -> std::optional<uid_t> {
  if (value == nullptr || value[0] == '\0') {
    return std::nullopt;
  }

  errno = 0;
  char *end = nullptr;
  const auto raw_uid = std::strtoul(value, &end, 10);
  if (errno != 0 || end == value || end == nullptr || *end != '\0' ||
      raw_uid > std::numeric_limits<uid_t>::max()) {
    return std::nullopt;
  }

  return static_cast<uid_t>(raw_uid);
}

auto parse_gid_env(const char *value) -> std::optional<gid_t> {
  if (value == nullptr || value[0] == '\0') {
    return std::nullopt;
  }

  errno = 0;
  char *end = nullptr;
  const auto raw_gid = std::strtoul(value, &end, 10);
  if (errno != 0 || end == value || end == nullptr || *end != '\0' ||
      raw_gid > std::numeric_limits<gid_t>::max()) {
    return std::nullopt;
  }

  return static_cast<gid_t>(raw_gid);
}

auto invoking_user_from_pwd(const passwd &pwd, gid_t gid_override)
    -> InvokingUser {
  return InvokingUser{
      .uid = pwd.pw_uid,
      .gid = gid_override,
      .name = pwd.pw_name,
      .home = pwd.pw_dir != nullptr ? pwd.pw_dir : "",
      .shell = pwd.pw_shell != nullptr ? pwd.pw_shell : "",
  };
}

auto resolve_invoking_user() -> std::optional<InvokingUser> {
  if (const auto sudo_uid = parse_uid_env(std::getenv("SUDO_UID"))) {
    if (passwd *pwd = getpwuid(*sudo_uid); pwd != nullptr) {
      const auto sudo_gid =
          parse_gid_env(std::getenv("SUDO_GID")).value_or(pwd->pw_gid);
      return invoking_user_from_pwd(*pwd, sudo_gid);
    }
  }

  if (const char *doas_user = std::getenv("DOAS_USER");
      doas_user != nullptr && doas_user[0] != '\0') {
    if (passwd *pwd = getpwnam(doas_user); pwd != nullptr) {
      return invoking_user_from_pwd(*pwd, pwd->pw_gid);
    }
  }

  if (const auto pkexec_uid = parse_uid_env(std::getenv("PKEXEC_UID"))) {
    if (passwd *pwd = getpwuid(*pkexec_uid); pwd != nullptr) {
      return invoking_user_from_pwd(*pwd, pwd->pw_gid);
    }
  }

  return std::nullopt;
}

void set_gui_env_var(const char *name, const std::string &value) {
  if (value.empty()) {
    unsetenv(name);
    return;
  }
  setenv(name, value.c_str(), 1);
}

void reset_gui_environment(const InvokingUser &invoking_user) {
  set_gui_env_var("HOME", invoking_user.home);
  set_gui_env_var("LOGNAME", invoking_user.name);
  set_gui_env_var("USER", invoking_user.name);
  set_gui_env_var("SHELL", invoking_user.shell);

  unsetenv("XDG_CONFIG_HOME");
  unsetenv("XDG_CACHE_HOME");
  unsetenv("XDG_DATA_HOME");
  unsetenv("XDG_STATE_HOME");

  const auto runtime_dir =
      std::filesystem::path("/run/user") / std::to_string(invoking_user.uid);
  if (std::filesystem::is_directory(runtime_dir)) {
    set_gui_env_var("XDG_RUNTIME_DIR", runtime_dir.string());

    const auto session_bus = runtime_dir / "bus";
    if (std::filesystem::exists(session_bus)) {
      set_gui_env_var("DBUS_SESSION_BUS_ADDRESS",
                      "unix:path=" + session_bus.string());
    }
  }

  if (std::getenv("XAUTHORITY") == nullptr && !invoking_user.home.empty()) {
    const auto xauthority =
        std::filesystem::path(invoking_user.home) / ".Xauthority";
    if (std::filesystem::is_regular_file(xauthority)) {
      set_gui_env_var("XAUTHORITY", xauthority.string());
    }
  }
}

auto drop_to_invoking_gui_user() -> bool {
  if (geteuid() != 0) {
    return true;
  }

  const auto invoking_user = resolve_invoking_user();
  if (!invoking_user.has_value()) {
    return false;
  }

  reset_gui_environment(*invoking_user);
  return initgroups(invoking_user->name.c_str(), invoking_user->gid) == 0 &&
         setgid(invoking_user->gid) == 0 && setuid(invoking_user->uid) == 0;
}

}  // namespace

int test_main(int argc, char **argv) {
  const TestArgs args = parse_args(argc, argv);
  const std::string config_path = howdy::native::resolve_config_path().string();
  const auto config_security =
      howdy::native::check_secure_root_owned_file(config_path, "Config file");
  if (!config_security.ok) {
    std::cerr << config_security.error_message << "\n";
    return kExitCameraError;
  }

  howdy::native::ConfigReader config(config_path);
  if (!config.ok()) {
    std::cerr << "Failed to read config file: " << config_path << "\n";
    return kExitCameraError;
  }

  howdy::native::FaceModel face_model(config);
  if (!face_model.ok()) {
    std::cerr << face_model.error_message() << "\n";
    return kExitCameraError;
  }

  auto loaded_models = howdy::native::UserModelLoadResult{};
  if (!args.user.empty()) {
    loaded_models = howdy::native::load_user_models(
        args.user, howdy::native::FaceModel::kBackendName);
    if (loaded_models.status ==
        howdy::native::UserModelStatus::kIncompatibleBackend) {
      std::cout
          << "Warning: Stored face models use an incompatible backend; matching disabled\n";
    } else if (loaded_models.status ==
               howdy::native::UserModelStatus::kInvalidUser) {
      std::cout << "Warning: Invalid user name; matching disabled\n";
    } else if (loaded_models.status == howdy::native::UserModelStatus::kNoModel) {
      std::cout << "Warning: No face model found for this user, detection will run without matching\n";
    } else if (loaded_models.status ==
               howdy::native::UserModelStatus::kParseError) {
      std::cout << "Warning: Failed to read stored face models, detection will run without matching\n";
    } else if (loaded_models.status ==
               howdy::native::UserModelStatus::kInsecurePath) {
      std::cout << "Warning: Stored face model path is insecure, detection will run without matching\n";
    }
  }

  auto settings = howdy::native::load_capture_settings(config);
  if (!args.device_path.empty()) {
    settings.device_path = args.device_path;
  }

  howdy::native::VideoCapture capture(settings);
  if (!capture.open()) {
    std::cerr << "Failed to open camera device: " << settings.device_path << "\n";
    std::cerr << "Error: " << capture.error_message() << "\n";
    return kExitCameraError;
  }

  const int exposure = howdy::native::config_exposure(config);
  const float dark_threshold = howdy::native::config_dark_threshold(config);
  const bool use_clahe = config.get_bool("video", "clahe_enabled", true);
  const auto clip_limit = howdy::native::config_clahe_clip_limit(config);
  const auto tile_size = howdy::native::config_clahe_tile_grid_size(config);

  cv::Ptr<cv::CLAHE> clahe;
  if (use_clahe) {
    clahe = cv::createCLAHE(clip_limit, cv::Size(tile_size, tile_size));
  }

  std::cout << "\nOpening a window with a test feed\n\n";
  std::cout << "Press ctrl+C in this terminal to quit\n";
  std::cout << "Click on the image to enable or disable slow mode\n\n";

  if (!drop_to_invoking_gui_user()) {
    std::cerr << "Failed to switch GUI session to the invoking user\n";
    std::cerr << "Run this command from your desktop session through sudo/doas/pkexec\n";
    capture.release();
    return kExitCameraError;
  }

  cv::namedWindow(kWindowName);
  cv::setMouseCallback(kWindowName, mouse_callback);

  int total_frames = 0;
  int sec_frames = 0;
  int fps = 0;
  auto second_anchor = std::chrono::steady_clock::now();
  double recognition_ms = 0.0;

  try {
    while (true) {
      const auto frame_start = std::chrono::steady_clock::now();
      total_frames++;
      sec_frames++;

      if (std::chrono::duration_cast<std::chrono::seconds>(
              frame_start - second_anchor)
              .count() >= 1) {
        fps = sec_frames;
        sec_frames = 0;
        second_anchor = frame_start;
      }

      cv::Mat frame;
      cv::Mat gray_frame;
      if (!capture.read(frame, &gray_frame)) {
        std::cerr << "Failed to read frame from camera\n";
        capture.release();
        return kExitCameraError;
      }

      if (use_clahe) {
        clahe->apply(gray_frame, gray_frame);
      }

      cv::Mat overlay;
      cv::cvtColor(gray_frame.clone(), overlay, cv::COLOR_GRAY2BGR);
      const int height = gray_frame.rows;
      const int width = gray_frame.cols;

      cv::Mat hist;
      constexpr std::array<int, 1> hist_size{8};
      constexpr std::array<float, 2> hist_range{0.0F, 256.0F};
      std::vector<const float *> ranges{hist_range.data()};
      constexpr std::array<int, 1> channels{0};
      cv::calcHist(&gray_frame, 1, channels.data(), cv::Mat(), hist, 1,
                   hist_size.data(), ranges.data());

      const auto hist_total = static_cast<float>(cv::sum(hist)[0]);
      std::vector<float> hist_perc;
      hist_perc.reserve(8);
      for (int index = 0; index < hist.rows; ++index) {
        const float value_perc =
            hist.at<float>(index, 0) / std::max(hist_total, 1.0F) * 100.0F;
        hist_perc.push_back(value_perc);
        const cv::Point p1(20 + (10 * index), 10);
        const cv::Point p2(10 + (10 * index),
                           static_cast<int>((value_perc / 2.0F) + 10.0F));
        cv::rectangle(overlay, p1, p2, cv::Scalar(0, 200, 0), cv::FILLED);
      }

      print_text(overlay, 0, height,
                 "RESOLUTION: " + std::to_string(height) + "x" +
                     std::to_string(width));
      print_text(overlay, 1, height, "FPS: " + std::to_string(fps));
      print_text(overlay, 2, height,
                 "FRAMES: " + std::to_string(total_frames));
      print_text(overlay, 3, height,
                 "RECOGNITION: " + std::to_string(static_cast<int>(recognition_ms)) +
                     "ms");
      print_text(overlay, 4, height, "BACKEND: OpenCV YuNet/SFace");
      print_text(overlay, 5, height,
                 std::string("CLAHE: ") + (use_clahe ? "on" : "off"));

      if (g_slow_mode) {
        cv::putText(overlay, "SLOW MODE", cv::Point(width - 66, height - 10),
                    cv::FONT_HERSHEY_SIMPLEX, 0.3, cv::Scalar(0, 0, 255), 0,
                    cv::LINE_AA);
      }

      if (!hist_perc.empty() && hist_perc[0] > dark_threshold) {
        cv::putText(overlay, "DARK FRAME", cv::Point(width - 68, 16),
                    cv::FONT_HERSHEY_SIMPLEX, 0.3, cv::Scalar(0, 0, 255), 0,
                    cv::LINE_AA);
      } else {
        cv::putText(overlay, "SCAN FRAME", cv::Point(width - 68, 16),
                    cv::FONT_HERSHEY_SIMPLEX, 0.3, cv::Scalar(0, 255, 0), 0,
                    cv::LINE_AA);

        const auto recognition_start = std::chrono::steady_clock::now();
        auto face_frame = face_model.prepare_frame(gray_frame);
        const auto face_locations = face_model.detect(face_frame);
        recognition_ms =
            static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() -
                                    recognition_start)
                                    .count());

        for (const auto &face : face_locations) {
          cv::Scalar color(0, 0, 230);
          const auto [x, y, w, h] = face_model.detection_box(face);
          const float confidence = face_model.detection_confidence(face);

          if (loaded_models.status == howdy::native::UserModelStatus::kOk) {
            const auto face_encoding = face_model.encode(face_frame, face);
            const auto match =
                face_model.best_match(loaded_models.stored.encodings, face_encoding);

            std::string face_text;
            if (match.accepted) {
              color = cv::Scalar(0, 230, 0);
              const auto &model =
                  loaded_models.stored.models[static_cast<std::size_t>(match.index)];
              face_text =
                  model.label + " (score: " + cv::format("%.3f", match.score) + ")";
            } else {
              face_text = "no match (" + cv::format("%.3f", match.score) + ")";
            }

            cv::putText(overlay, face_text, cv::Point(x, std::max(0, y - 8)),
                        cv::FONT_HERSHEY_SIMPLEX, 0.3, color, 0, cv::LINE_AA);
          }

          cv::rectangle(overlay, cv::Rect(x, y, w, h), color, 2);
          cv::putText(overlay, cv::format("%.2f", confidence),
                      cv::Point(x, std::min(height - 4, y + h + 12)),
                      cv::FONT_HERSHEY_SIMPLEX, 0.3, color, 0, cv::LINE_AA);
          for (const auto &point : face_model.detection_landmarks(face)) {
            cv::circle(overlay, point, 2, cv::Scalar(0, 255, 255), -1);
          }
        }
      }

      cv::Mat display_frame;
      cv::cvtColor(gray_frame, display_frame, cv::COLOR_GRAY2BGR);
      cv::addWeighted(overlay, 0.65, display_frame, 0.35, 0, display_frame);
      cv::imshow(kWindowName, display_frame);

      if (cv::waitKey(1) != -1) {
        break;
      }

      const auto frame_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - frame_start)
                                  .count();
      if (g_slow_mode) {
        const auto sleep_ms = std::max(0LL, 500LL - frame_time);
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
      }

      if (exposure != -1) {
        capture.set(cv::CAP_PROP_AUTO_EXPOSURE, 1.0);
        capture.set(cv::CAP_PROP_EXPOSURE, static_cast<double>(exposure));
      }
    }
  } catch (...) {
    cv::destroyAllWindows();
    capture.release();
    throw;
  }

  cv::destroyAllWindows();
  capture.release();
  return kExitOk;
}
