#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "config/config_reader.hpp"

namespace howdy::native {

auto normalized_lower(std::string value) -> std::string;
auto parse_int_strict(std::string_view value) -> std::optional<int>;
auto parse_float_strict(std::string_view value) -> std::optional<float>;
auto is_valid_bool_text(std::string_view value) -> bool;
auto invalid_config_value_message(std::string_view key, std::string_view rule)
    -> std::string;
auto validate_known_config_value(const ConfigReader &config,
                                 std::string_view key,
                                 std::string_view value)
    -> std::optional<std::string>;
auto validate_runtime_config(const ConfigReader &config)
    -> std::optional<std::string>;

}  // namespace howdy::native
