#pragma once

#include <optional>
#include <string>

#include "config/config_reader.hpp"

namespace howdy::native {

auto validate_runtime_config(const ConfigReader &config)
    -> std::optional<std::string>;

}  // namespace howdy::native
