#pragma once

#include "common/file_security.hpp"

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#include <sys/types.h>

namespace howdy::native {

    enum class OpenCvModelStatus {
        kOk,
        kMissing,
        kInsecure,
        kInvalid,
    };

    struct OpenCvModelReadiness {
        OpenCvModelStatus status = OpenCvModelStatus::kInsecure;
        std::string       error_message;
    };

    inline auto is_invalid_model_file(const std::filesystem::path &path) -> bool {
        if (!std::filesystem::is_regular_file(path)) {
            return true;
        }

        std::ifstream input(path, std::ios::binary);
        if (!input.is_open()) {
            return true;
        }

        std::string header(256, '\0');
        input.read(header.data(), static_cast<std::streamsize>(header.size()));
        header.resize(static_cast<std::size_t>(input.gcount()));
        return header.starts_with("version https://git-lfs.github.com/spec/v1") ||
               (!header.empty() && header.front() == '<');
    }

    inline auto check_opencv_model_readiness_with_label(const std::filesystem::path &path,
                                                        const std::string_view       label,
                                                        const std::optional<uid_t>   owner_uid)
        -> OpenCvModelReadiness {
        const auto parent = path.parent_path();
        if (parent.empty()) {
            return OpenCvModelReadiness{
                .status = OpenCvModelStatus::kInsecure,
                .error_message =
                    std::string(label) + " must have a parent directory: " + path.string(),
            };
        }

        const auto directory_security =
            check_secure_root_owned_directory_tree(parent, "Models directory", owner_uid);
        if (!directory_security.ok) {
            return OpenCvModelReadiness{
                .status        = OpenCvModelStatus::kInsecure,
                .error_message = directory_security.error_message,
            };
        }

        const auto file_security = check_secure_root_owned_file(path, label, owner_uid);
        if (!file_security.ok) {
            if (file_security.error_code == ENOENT) {
                return OpenCvModelReadiness{
                    .status        = OpenCvModelStatus::kMissing,
                    .error_message = std::string(label) + " is missing: " + path.string(),
                };
            }
            return OpenCvModelReadiness{
                .status        = OpenCvModelStatus::kInsecure,
                .error_message = file_security.error_message,
            };
        }

        if (is_invalid_model_file(path)) {
            return OpenCvModelReadiness{
                .status        = OpenCvModelStatus::kInvalid,
                .error_message = std::string(label) + " is invalid: " + path.string(),
            };
        }

        return OpenCvModelReadiness{.status = OpenCvModelStatus::kOk, .error_message = {}};
    }

    inline auto check_opencv_face_model_readiness(const std::filesystem::path &path,
                                                  const std::optional<uid_t>   owner_uid)
        -> OpenCvModelReadiness {
        return check_opencv_model_readiness_with_label(path, "OpenCV face model file", owner_uid);
    }

}  // namespace howdy::native
