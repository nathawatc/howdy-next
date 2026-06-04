#include "config/config_reader.hpp"
#include "config/config_validation.hpp"
#include "config/config_values.hpp"
#include "config/number_parsing.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

    auto write_file(const std::filesystem::path &path, const std::string &content) -> bool {
        std::ofstream out(path);
        if (!out.is_open()) {
            return false;
        }
        out << content;
        return out.good();
    }

    auto expect(bool condition, const std::string &message) -> bool {
        if (!condition) {
            std::cerr << "FAIL: " << message << "\n";
            return false;
        }
        return true;
    }

    auto validates(const std::filesystem::path &path, const std::string &content) -> bool {
        if (!write_file(path, content)) {
            return false;
        }
        const howdy::native::ConfigReader config(path.string());
        return config.ok() && !howdy::native::validate_runtime_config(config).has_value();
    }

    auto rejects(const std::filesystem::path &path, const std::string &content,
                 const std::string &needle) -> bool {
        if (!write_file(path, content)) {
            return false;
        }
        const howdy::native::ConfigReader config(path.string());
        if (!config.ok()) {
            return false;
        }
        const auto validation = howdy::native::validate_runtime_config(config);
        return validation.has_value() && validation->find(needle) != std::string::npos;
    }

}  // namespace

auto main() -> int {
    namespace fs = std::filesystem;

    bool            ok        = true;
    const auto      temp_root = fs::current_path() / "howdy-config-validation-test";
    std::error_code ec;
    fs::remove_all(temp_root, ec);
    fs::create_directories(temp_root, ec);
    ok &= expect(!ec, "create temp root");

    ok &= expect(validates(temp_root / "device-fps-zero.ini", "[video]\ndevice_fps = 0\n"),
                 "device_fps zero is valid");
    ok &= expect(
        rejects(temp_root / "device-fps-negative.ini", "[video]\ndevice_fps = -1\n", "device_fps"),
        "device_fps negative is invalid");
    ok &= expect(
        rejects(temp_root / "device-fps-text.ini", "[video]\ndevice_fps = fast\n", "device_fps"),
        "invalid integer values are rejected");
    ok &= expect(rejects(temp_root / "float-text.ini", "[video]\nclahe_clip_limit = fast\n",
                         "clahe_clip_limit"),
                 "invalid float values are rejected");

    for (const auto *const value : {"nan", "+inf", "-inf"}) {
        ok &= expect(rejects(temp_root / (std::string("non-finite-") + value + ".ini"),
                             "[face]\nyunet_score_threshold = " + std::string(value) + "\n",
                             "yunet_score_threshold"),
                     std::string("non-finite float is rejected: ") + value);
        ok &= expect(!howdy::native::parse_config_float_strict(value).has_value(),
                     std::string("strict parser rejects non-finite value: ") + value);
    }

    ok &= expect(validates(temp_root / "cosine-threshold-min.ini",
                           "[face]\nsface_metric = cosine\nsface_threshold = 0\n"),
                 "cosine threshold minimum boundary is valid");
    ok &= expect(validates(temp_root / "cosine-threshold-max.ini",
                           "[face]\nsface_metric = cosine\nsface_threshold = 1\n"),
                 "cosine threshold maximum boundary is valid");
    ok &= expect(rejects(temp_root / "cosine-threshold-over.ini",
                         "[face]\nsface_metric = cosine\nsface_threshold = 1.001\n",
                         "sface_threshold"),
                 "cosine threshold above maximum is invalid");
    ok &= expect(validates(temp_root / "l2-threshold-max.ini",
                           "[face]\nsface_metric = l2\nsface_threshold = 4\n"),
                 "l2 threshold maximum boundary is valid");
    ok &= expect(rejects(temp_root / "l2-threshold-over.ini",
                         "[face]\nsface_metric = l2\nsface_threshold = 4.001\n", "sface_threshold"),
                 "l2 threshold above maximum is invalid");

    ok &= expect(validates(temp_root / "absolute-model-path.ini",
                           "[face]\nyunet_model = /opt/howdy/yunet.onnx\n"
                           "sface_model = /opt/howdy/sface.onnx\n"),
                 "absolute custom model paths are accepted by validation");
    ok &= expect(rejects(temp_root / "relative-yunet-path.ini",
                         "[face]\nyunet_model = models/yunet.onnx\n", "yunet_model"),
                 "relative YuNet model path is rejected");
    ok &= expect(rejects(temp_root / "relative-sface-path.ini",
                         "[face]\nsface_model = models/sface.onnx\n", "sface_model"),
                 "relative SFace model path is rejected");

    ok &= expect(validates(temp_root / "obsolete-keys.ini",
                           "[core]\nworkaround = input\ngtk_stdout = true\n"),
                 "obsolete workaround and gtk_stdout config keys remain ignored");

    const auto invalid_runtime = temp_root / "invalid-runtime.ini";
    ok &= expect(write_file(invalid_runtime, "[video]\ntimeout = abc\n"), "write invalid runtime");
    const howdy::native::ConfigReader invalid_config(invalid_runtime.string());
    ok &= expect(invalid_config.ok(), "invalid runtime config is syntactically parseable");
    ok &= expect(howdy::native::validate_runtime_config(invalid_config).has_value(),
                 "invalid runtime config fails closed before runtime use");
    ok &= expect(howdy::native::config_timeout_seconds(invalid_config) == 4,
                 "unsafe invalid timeout falls back to current default value");

    fs::remove_all(temp_root, ec);

    return ok ? 0 : 1;
}
