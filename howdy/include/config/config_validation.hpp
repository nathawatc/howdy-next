#pragma once

#include "config/config_reader.hpp"

#include <optional>
#include <string>

namespace howdy::native {

	auto validate_runtime_config(const ConfigReader &config) -> std::optional<std::string>;

}  // namespace howdy::native
