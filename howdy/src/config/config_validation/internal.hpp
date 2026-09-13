#pragma once

#include <optional>
#include <string>

namespace howdy::native {

	class ConfigReader;

	auto ValidateRuntimeConfig(const ConfigReader &config) -> std::optional<std::string>;

}  // namespace howdy::native
