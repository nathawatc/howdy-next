#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace howdy::native {

	class ConfigDocument;

	auto InvalidConfigValueMessage(std::string_view key, std::string_view value,
	                               std::string_view rule) -> std::string;

	auto ValidateRuntimeConfig(const ConfigDocument &config) -> std::optional<std::string>;

}  // namespace howdy::native
