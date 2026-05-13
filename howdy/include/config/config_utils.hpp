#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace howdy::native {

auto read_config_lines(const std::filesystem::path &config_path, bool lock = false)
    -> std::vector<std::string>;
auto atomic_write_lines(const std::filesystem::path &config_path,
                        const std::vector<std::string> &lines) -> bool;
auto update_config_value(const std::filesystem::path &config_path,
                         const std::string &key, const std::string &value,
                         bool lock = false) -> bool;

}  // namespace howdy::native
