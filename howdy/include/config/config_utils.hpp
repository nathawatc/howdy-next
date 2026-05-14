#pragma once

#include "common/file_security.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace howdy::native {

struct ConfigPathCheckResult {
  bool ok = false;
  std::string error_message;
};

inline auto check_secure_config_path(const std::filesystem::path &config_path)
    -> ConfigPathCheckResult {
  const auto parent = config_path.parent_path();
  if (parent.empty()) {
    return ConfigPathCheckResult{
        .ok = false,
        .error_message = "Config file must have a parent directory: " +
                         config_path.string(),
    };
  }

  const auto file_security = check_secure_root_owned_file_with_directory(
      config_path, "Config directory", "Config file");
  if (!file_security.ok) {
    return ConfigPathCheckResult{
        .ok = false,
        .error_message = file_security.error_message,
    };
  }

  return ConfigPathCheckResult{.ok = true, .error_message = {}};
}
auto is_safe_ini_scalar_value(std::string_view value) -> bool;
auto read_config_lines(const std::filesystem::path &config_path, bool lock = false)
    -> std::vector<std::string>;
auto atomic_write_lines(const std::filesystem::path &config_path,
                        const std::vector<std::string> &lines) -> bool;
auto update_config_value(const std::filesystem::path &config_path,
                         const std::string &key, const std::string &value,
                         std::string *error_message,
                         bool lock = false) -> bool;
inline auto update_config_value(const std::filesystem::path &config_path,
                                const std::string &key,
                                const std::string &value, bool lock = false)
    -> bool {
  return update_config_value(config_path, key, value, nullptr, lock);
}

}  // namespace howdy::native
