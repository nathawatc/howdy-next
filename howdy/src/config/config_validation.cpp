#include "config/config_validation.hpp"

#include "common/capture_device_path.hpp"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace howdy::native {
    namespace {

        auto normalized_lower(std::string value) -> std::string {
            for (char &ch : value) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            return value;
        }

        auto parse_int_strict(std::string_view value) -> std::optional<int> {
            if (value.empty()) {
                return std::nullopt;
            }

            errno                 = 0;
            char             *end = nullptr;
            const std::string owned_value(value);
            const auto        parsed = std::strtol(owned_value.c_str(), &end, 10);
            if (errno != 0 || end == nullptr || *end != '\0' ||
                parsed < std::numeric_limits<int>::min() ||
                parsed > std::numeric_limits<int>::max()) {
                return std::nullopt;
            }

            return static_cast<int>(parsed);
        }

        auto parse_float_strict(std::string_view value) -> std::optional<float> {
            if (value.empty()) {
                return std::nullopt;
            }

            errno                 = 0;
            char             *end = nullptr;
            const std::string owned_value(value);
            const auto        parsed = std::strtof(owned_value.c_str(), &end);
            if (errno != 0 || end == nullptr || *end != '\0' || !std::isfinite(parsed)) {
                return std::nullopt;
            }

            return parsed;
        }

        auto is_valid_bool_text(std::string_view value) -> bool {
            const auto lowered = normalized_lower(std::string(value));
            return lowered == "true" || lowered == "false" || lowered == "1" || lowered == "0" ||
                   lowered == "yes" || lowered == "no" || lowered == "on" || lowered == "off";
        }

        auto invalid_config_value_message(std::string_view key, std::string_view value,
                                          std::string_view rule) -> std::string {
            return "Invalid config value for " + std::string(key) + "=\"" + std::string(value) +
                   "\": " + std::string(rule);
        }

        auto validate_known_config_value(const ConfigReader &config, std::string_view key,
                                         std::string_view value) -> std::optional<std::string> {
            if (key == "detection_notice" || key == "no_confirmation" || key == "abort_if_ssh" ||
                key == "abort_if_lid_closed" || key == "disabled" || key == "warn_no_device" ||
                key == "clahe_enabled" || key == "force_mjpeg" || key == "save_failed" ||
                key == "save_successful" || key == "end_report") {
                if (!is_valid_bool_text(value)) {
                    return invalid_config_value_message(key, value, "expected a boolean");
                }
                return std::nullopt;
            }

            if (key == "device_path") {
                if (value.empty() || !is_allowed_capture_device_path(value)) {
                    return invalid_config_value_message(
                        key, value, "expected none, /dev/video*, or /dev/v4l/by-path/*");
                }
                return std::nullopt;
            }

            if (key == "sface_metric") {
                const auto lowered = normalized_lower(std::string(value));
                if (lowered != "cosine" && lowered != "l2" && lowered != "l2norm") {
                    return invalid_config_value_message(key, value,
                                                        "expected one of: cosine, l2, l2norm");
                }
                return std::nullopt;
            }

            if (key == "yunet_model" || key == "sface_model") {
                if (value.empty()) {
                    return invalid_config_value_message(key, value, "must not be empty");
                }
                if (value == "default" || value == "none") {
                    return std::nullopt;
                }
                if (!std::filesystem::path(std::string(value)).is_absolute()) {
                    return invalid_config_value_message(
                        key, value, "expected an absolute path, default, or none");
                }
                return std::nullopt;
            }

            auto validate_int_range = [&](int minimum, int maximum,
                                          std::string_view rule) -> std::optional<std::string> {
                const auto parsed = parse_int_strict(value);
                if (!parsed.has_value() || *parsed < minimum || *parsed > maximum) {
                    return invalid_config_value_message(key, value, rule);
                }
                return std::nullopt;
            };

            auto validate_float_range = [&](float minimum, float maximum,
                                            std::string_view rule) -> std::optional<std::string> {
                const auto parsed = parse_float_strict(value);
                if (!parsed.has_value() || *parsed < minimum || *parsed > maximum) {
                    return invalid_config_value_message(key, value, rule);
                }
                return std::nullopt;
            };

            if (key == "timeout") {
                return validate_int_range(1, 300, "expected integer range 1..300");
            }
            if (key == "max_height") {
                return validate_float_range(32.0F, 4096.0F, "expected range 32..4096");
            }
            if (key == "rotate") {
                return validate_int_range(0, 2, "expected integer range 0..2");
            }
            if (key == "dark_threshold") {
                return validate_float_range(0.0F, 99.9F, "expected range 0..99.9");
            }
            if (key == "clahe_clip_limit") {
                return validate_float_range(0.01F, 100.0F, "expected range 0.01..100");
            }
            if (key == "clahe_tile_grid_size") {
                return validate_int_range(1, 64, "expected integer range 1..64");
            }
            if (key == "yunet_score_threshold" || key == "yunet_nms_threshold") {
                return validate_float_range(0.0F, 1.0F, "expected range 0..1");
            }
            if (key == "yunet_top_k") {
                return validate_int_range(1, 10000, "expected integer range 1..10000");
            }

            if (key == "frame_width" || key == "frame_height") {
                const auto parsed = parse_int_strict(value);
                if (!parsed.has_value() || (*parsed != -1 && (*parsed < 16 || *parsed > 8192))) {
                    return invalid_config_value_message(key, value,
                                                        "expected -1 or integer range 16..8192");
                }
                return std::nullopt;
            }

            if (key == "device_fps") {
                const auto parsed = parse_int_strict(value);
                if (!parsed.has_value() || *parsed < 0 || *parsed > 480) {
                    return invalid_config_value_message(key, value,
                                                        "expected integer range 0..480");
                }
                return std::nullopt;
            }

            if (key == "exposure") {
                const auto parsed = parse_int_strict(value);
                if (!parsed.has_value() || (*parsed != -1 && (*parsed < 0 || *parsed > 10000))) {
                    return invalid_config_value_message(key, value,
                                                        "expected -1 or integer range 0..10000");
                }
                return std::nullopt;
            }

            if (key == "sface_threshold") {
                const auto parsed = parse_float_strict(value);
                if (!parsed.has_value()) {
                    return invalid_config_value_message(key, value,
                                                        "expected a floating-point value");
                }
                const auto  metric = normalized_lower(config.get("face", "sface_metric", "cosine"));
                const float maximum = metric == "cosine" ? 1.0F : 4.0F;
                if (*parsed < 0.0F || *parsed > maximum) {
                    return invalid_config_value_message(key, value,
                                                        metric == "cosine" ? "expected range 0..1"
                                                                           : "expected range 0..4");
                }
                return std::nullopt;
            }

            return std::nullopt;
        }

    }  // namespace

    auto validate_runtime_config(const ConfigReader &config) -> std::optional<std::string> {
        const std::vector<std::pair<const char *, const char *>> keys = {
            {"core", "detection_notice"},
            {"core", "no_confirmation"},
            {"core", "abort_if_ssh"},
            {"core", "abort_if_lid_closed"},
            {"core", "disabled"},
            {"video", "timeout"},
            {"video", "device_path"},
            {"video", "warn_no_device"},
            {"video", "max_height"},
            {"video", "frame_width"},
            {"video", "frame_height"},
            {"video", "clahe_enabled"},
            {"video", "clahe_clip_limit"},
            {"video", "clahe_tile_grid_size"},
            {"video", "dark_threshold"},
            {"video", "force_mjpeg"},
            {"video", "exposure"},
            {"video", "device_fps"},
            {"video", "rotate"},
            {"face", "yunet_model"},
            {"face", "sface_model"},
            {"face", "yunet_score_threshold"},
            {"face", "yunet_nms_threshold"},
            {"face", "yunet_top_k"},
            {"face", "sface_metric"},
            {"face", "sface_threshold"},
            {"snapshots", "save_failed"},
            {"snapshots", "save_successful"},
            {"debug", "end_report"},
        };

        for (const auto &[section, key] : keys) {
            const auto value = config.get(section, key, "");
            if (value.empty()) {
                continue;
            }
            if (const auto validation = validate_known_config_value(config, key, value)) {
                return validation;
            }
        }

        return std::nullopt;
    }

}  // namespace howdy::native
