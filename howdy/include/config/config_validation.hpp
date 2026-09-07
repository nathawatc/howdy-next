#pragma once

#include "config/config_reader.hpp"

#include <optional>
#include <string>

namespace howdy::native {

	auto ValidateRuntimeConfig(const ConfigReader &config) -> std::optional<std::string>;

}  // namespace howdy::native
