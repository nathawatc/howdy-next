#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace howdy::native {

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
