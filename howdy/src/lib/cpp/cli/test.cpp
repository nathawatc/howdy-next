#include "test_cli.hpp"

#include "../recorders/video_capture.hpp"
#include "../config/config_reader.hpp"
#include "../config/runtime_paths.hpp"

#include <cstring>
#include <iostream>
#include <thread>
#include <chrono>

static constexpr int EXIT_OK = 0;
static constexpr int EXIT_CAMERA_ERROR = 1;

struct TestArgs {
    std::string device_path = "/dev/video0";
};

static TestArgs parse_args(int argc, char* argv[]) {
    TestArgs args;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            args.device_path = argv[++i];
        }
    }

    return args;
}

int test_main(int argc, char* argv[]) {
    TestArgs args = parse_args(argc, argv);

    std::string config_path = howdy::native::resolve_config_path().string();

    howdy::native::ConfigReader config(config_path);
    if (!config.ok()) {
        std::cerr << "Failed to read config file: " << config_path << "\n";
        return EXIT_CAMERA_ERROR;
    }

    howdy::native::CaptureSettings settings;
    settings.device_path = args.device_path;
    settings.frame_width = config.get_int("video", "frame_width", -1);
    settings.frame_height = config.get_int("video", "frame_height", -1);
    settings.device_fps = config.get_int("video", "device_fps", 0);
    settings.force_mjpeg = config.get_bool("video", "force_mjpeg", false);

    howdy::native::VideoCapture capture(settings);

    if (!capture.open()) {
        std::cerr << "Failed to open camera device: " << args.device_path << "\n";
        std::cerr << "Error: " << capture.error_message() << "\n";
        return EXIT_CAMERA_ERROR;
    }

    std::cout << "Opening a window with a test feed\n\n";
    std::cout << "Press ctrl+C in this terminal to quit\n";
    std::cout << "Click on the image to enable or disable slow mode\n\n";

    cv::Mat frame;
    int frame_count = 0;
    int fps = 0;
    auto last_fps_time = std::chrono::steady_clock::now();
    int frames_since_last_fps = 0;

    constexpr int MAX_FRAMES = 60;
    constexpr auto frame_delay = std::chrono::milliseconds(100);

    while (frame_count < MAX_FRAMES) {
        if (!capture.read(frame)) {
            std::cerr << "Failed to read frame from camera\n";
            capture.release();
            return EXIT_CAMERA_ERROR;
        }

        frame_count++;
        frames_since_last_fps++;

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_fps_time).count();
        if (elapsed >= 1000) {
            fps = frames_since_last_fps * 1000 / elapsed;
            frames_since_last_fps = 0;
            last_fps_time = now;
        }

        int width = 0;
        int height = 0;
        if (!frame.empty()) {
            width = frame.cols;
            height = frame.rows;
        }

        std::cout << "\rFPS: " << fps << " | Frames: " << frame_count
                  << " | Resolution: " << width << "x" << height
                  << " | Plugin: OpenCV YuNet/SFace     " << std::flush;

        std::this_thread::sleep_for(frame_delay);
    }

    std::cout << "\n\nTest completed successfully\n";
    capture.release();
    return EXIT_OK;
}
