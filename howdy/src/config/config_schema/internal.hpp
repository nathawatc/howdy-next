#pragma once

#include "config/config_schema.hpp"

#include <optional>
#include <span>
#include <string>

namespace howdy::native::config_schema_internal {

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	validate_schema_options(std::span<const config_schema::Option> options)
	    -> std::optional<std::string>;

}  // namespace howdy::native::config_schema_internal
