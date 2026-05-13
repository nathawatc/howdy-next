#include "add_cli.hpp"

#include <nlohmann/json.hpp>
#include "../exported_headers/face_model.hpp"
#include "../exported_headers/video_capture.hpp"
#include "../core/image_utils.hpp"
#include "../recorders/opencv_capture.hpp"
#include "../recorders/ffmpeg_reader.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>
#include <cstring>
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <chrono>
#include <filesystem>

namespace fs = std::filesystem;

static constexpr int EXIT_SUCCESS = 0;
static constexpr int EXIT_ABORT = 1;
static constexpr const char* DEFAULT_CONFIG_PATH = "/lib/security/howdy/config.ini";
static constexpr float DEFAULT_DARK_THRESHOLD = 60.0f;
static constexpr int MAX_FRAMES = 60;
static constexpr const char* BACKEND_NAME = "opencv_dnn_sface";

struct AddArgs {
    std::string username;
    std::string label;
    bool plain = false;
    bool yes = false;
};

static AddArgs parse_args(int argc, char* argv[]) {
    AddArgs args;

    if (argc < 2) {
        std::cerr << "Usage: add <username> [--label <label>] [--plain] [-y]\n";
        std::exit(EXIT_ABORT);
    }

    args.username = argv[1];

    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--label") == 0 && i + 1 < argc) {
            args.label = argv[++i];
        } else if (strcmp(argv[i], "--plain") == 0) {
            args.plain = true;
        } else if (strcmp(argv[i], "-y") == 0) {
            args.yes = true;
        }
    }

    return args;
}

static std::string get_user_models_dir() {
    return "/lib/security/howdy/user_models";
}

static std::string get_user_model_path(const std::string& username) {
    return get_user_models_dir() + "/" + username + ".json";
}

static bool ensure_user_models_dir() {
    std::string dir = get_user_models_dir();
    if (!fs::exists(dir)) {
        std::cerr << "No face model folder found, creating one\n";
        try {
            fs::create_directories(dir);
            return true;
        } catch (const fs::filesystem_error& e) {
            std::cerr << "Failed to create directory: " << e.what() << "\n";
            return false;
        }
    }
    return true;
}

static std::vector<nlohmann::json> load_existing_models(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        return {};
    }
    try {
        return nlohmann::json::parse(f);
    } catch (const nlohmann::json::parse_error&) {
        return {};
    }
}

static bool has_incompatible_backend(const std::vector<nlohmann::json>& models) {
    for (const auto& model : models) {
        if (model.contains("backend")) {
            std::string backend = model["backend"];
            if (backend != BACKEND_NAME) {
                return true;
            }
        }
    }
    return false;
}

static std::string get_current_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ) % 1000;

    std::tm tm_buf;
    gmtime_r(&time_t, &tm_buf);

    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count() << "Z";
    return oss.str();
}

static nlohmann::json create_model_entry(
    const std::string& label,
    int next_id,
    const std::string& metric,
    const std::vector<float>& encoding
) {
    nlohmann::json entry;
    entry["id"] = next_id;
    entry["label"] = label;
    entry["backend"] = BACKEND_NAME;
    entry["metric"] = metric;
    entry["model"] = "face_recognition_sface_2021dec_int8bq.onnx";
    entry["time"] = static_cast<long long>(std::time(nullptr));
    entry["date"] = get_current_timestamp();
    entry["data"] = encoding;
    return entry;
}

static bool save_models_atomic(const std::string& path, const nlohmann::json& models) {
    std::string dir = fs::path(path).parent_path().string();
    std::string tmp_path = dir + "/.tmp_XXXXXX";

    int fd = mkstemp(const_cast<char*>(tmp_path.c_str()));
    if (fd == -1) {
        std::cerr << "Failed to create temp file\n";
        return false;
    }

    try {
        std::ofstream out(fd, std::ios::binary);
        if (!out.is_open()) {
            close(fd);
            unlink(tmp_path.c_str());
            std::cerr << "Failed to open temp file for writing\n";
            return false;
        }

        out << models.dump(2);
        out.close();
        close(fd);

        fs::rename(tmp_path, path);
        return true;
    } catch (const std::exception& e) {
        close(fd);
        unlink(tmp_path.c_str());
        std::cerr << "Failed to save models: " << e.what() << "\n";
        return false;
    }
}

static float calculate_darkness(const cv::Mat& gray_frame) {
    if (gray_frame.empty()) {
        return 100.0f;
    }

    cv::Mat hist;
    int histSize = 256;
    float range[] = {0, 256};
    const float* histRange = {range};
    cv::calcHist(&gray_frame, 1, nullptr, cv::noArray(), hist, 1, &histSize, &histRange);

    float hist_total = cv::sum(hist)[0];
    if (hist_total == 0) {
        return 100.0f;
    }

    float dark_pixels = hist.at<float>(0);
    return (dark_pixels / hist_total) * 100.0f;
}

int add_main(int argc, char* argv[]) {
    AddArgs args = parse_args(argc, argv);

    std::string config_path = getenv("HOWDY_CONFIG") ? getenv("HOWDY_CONFIG") : "";
    if (config_path.empty()) {
        config_path = DEFAULT_CONFIG_PATH;
    }

    std::string model_path = get_user_model_path(args.username);
    std::vector<nlohmann::json> models = load_existing_models(model_path);

    if (has_incompatible_backend(models)) {
        std::cerr << "Existing face models use an incompatible backend.\n";
        std::cerr << "Please run `howdy clear` and enroll again with `howdy add`.\n";
        return EXIT_ABORT;
    }

    if (models.size() > 3) {
        std::cerr << "NOTICE: Each additional model slows down the face recognition engine slightly\n";
        std::cerr << "Press Ctrl+C to cancel\n";
    }

    std::string label;
    int next_id = models.empty() ? 0 : (models.back()["id"].get<int>() + 1);

    if (!args.label.empty()) {
        label = args.label;
        if (!args.yes && !args.plain) {
            std::cout << "Using default label \"" << label << "\" because of -y flag\n";
        }
    } else {
        label = "Model #" + std::to_string(next_id);
    }

    if (!args.plain && !args.yes && args.label.empty()) {
        std::cout << "Enter a label for this new model [" << label << "]: ";
        std::string input;
        std::getline(std::cin, input);
        if (!input.empty()) {
            label = input.substr(0, 24);
            label.erase(std::remove(label.begin(), label.end(), '\n'), label.end());
            label.erase(std::remove(label.begin(), label.end(), '\r'), label.end());
        }
    }

    if (label.find(',') != std::string::npos) {
        std::cerr << "NOTICE: Removing illegal character \",\" from model name\n";
        label.erase(std::remove(label.begin(), label.end(), ','), label.end());
    }

    if (!ensure_user_models_dir()) {
        return EXIT_ABORT;
    }

    ConfigReader config(config_path);

    std::string device_path = config.get("video", "device_path", "/dev/video0");
    int frame_width = config.get_int("video", "frame_width", -1);
    int frame_height = config.get_int("video", "frame_height", -1);
    int device_fps = config.get_int("video", "device_fps", 0);
    bool force_mjpeg = config.get_bool("video", "force_mjpeg", false);
    std::string recording_plugin = config.get("video", "recording_plugin", "opencv");
    float dark_threshold = config.get_float("video", "dark_threshold", DEFAULT_DARK_THRESHOLD);
    bool clahe_enabled = config.get_bool("video", "clahe", false);

    auto video_capture = VideoCaptureFactory::create(
        device_path,
        recording_plugin,
        frame_width,
        frame_height,
        device_fps,
        force_mjpeg
    );

    if (!video_capture) {
        std::cerr << "Failed to create video capture\n";
        return EXIT_ABORT;
    }

    FaceModel face_model(config_path);

    ImageUtils clahe;
    if (clahe_enabled) {
        clahe.createCLAHE(2.0, cv::Size(8, 8));
    }

    if (!args.plain) {
        std::cerr << "\nPlease look straight into the camera\n";
    }

    std::this_thread::sleep_for(std::chrono::seconds(2));

    cv::Mat frame;
    std::vector<cv::Mat> face_locations;
    int frames = 0;
    int valid_frames = 0;
    int dark_tries = 0;
    float dark_running_total = 0.0f;

    while (frames < MAX_FRAMES && face_locations.empty()) {
        frames++;

        if (!video_capture->read(frame)) {
            continue;
        }

        cv::Mat gray_frame;
        if (frame.channels() == 3) {
            cv::cvtColor(frame, gray_frame, cv::COLOR_BGR2GRAY);
        } else {
            gray_frame = frame;
        }

        if (clahe_enabled) {
            clahe.apply(gray_frame);
        }

        float darkness = calculate_darkness(gray_frame);

        if (darkness > 99.99f) {
            valid_frames++;
            continue;
        }

        valid_frames++;
        dark_running_total += darkness;

        if (darkness > dark_threshold) {
            dark_tries++;
            continue;
        }

        cv::Mat prepared = face_model.prepare_frame(gray_frame);
        face_locations = face_model.detect(prepared);

        if (!face_locations.empty()) {
            break;
        }
    }

    video_capture->release();

    if (face_locations.empty()) {
        if (valid_frames == 0) {
            std::cerr << "Camera saw only black frames - is IR emitter working?\n";
        } else if (valid_frames == dark_tries) {
            std::cerr << "All frames were too dark, please check dark_threshold in config\n";
            float avg_darkness = (valid_frames > 0) ? (dark_running_total / valid_frames) : 0.0f;
            std::cerr << "Average darkness: " << avg_darkness
                      << ", Threshold: " << dark_threshold << "\n";
        } else {
            std::cerr << "No face detected, aborting\n";
        }
        return EXIT_ABORT;
    }

    if (face_locations.size() > 1) {
        std::cerr << "Multiple faces detected, aborting\n";
        return EXIT_ABORT;
    }

    if (frame.empty()) {
        std::cerr << "No valid frame captured, aborting\n";
        return EXIT_ABORT;
    }

    cv::Mat face_encoding = face_model.encode(frame, face_locations[0]);

    if (face_encoding.empty()) {
        std::cerr << "Failed to encode face, aborting\n";
        return EXIT_ABORT;
    }

    std::vector<float> encoding_vec;
    encoding_vec.reserve(128);
    if (face_encoding.cols >= 128) {
        for (int i = 0; i < 128; ++i) {
            encoding_vec.push_back(face_encoding.at<float>(0, i));
        }
    } else if (face_encoding.rows >= 128) {
        for (int i = 0; i < 128; ++i) {
            encoding_vec.push_back(face_encoding.at<float>(i, 0));
        }
    } else {
        for (int i = 0; i < face_encoding.total() && i < 128; ++i) {
            encoding_vec.push_back(face_encoding.at<float>(i));
        }
    }

    nlohmann::json new_entry = create_model_entry(
        label,
        next_id,
        face_model.metric(),
        encoding_vec
    );

    models.push_back(new_entry);

    if (!save_models_atomic(model_path, models)) {
        std::cerr << "Failed to save face model\n";
        return EXIT_ABORT;
    }

    std::cerr << "\nScan complete\n";
    std::cerr << "Added a new model to " << args.username << "\n";

    return EXIT_SUCCESS;
}