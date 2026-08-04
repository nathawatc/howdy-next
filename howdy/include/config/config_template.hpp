#pragma once

#include "config/config_schema.hpp"

#include <span>
#include <string>

namespace howdy::native::config_template {

	struct ConfigTemplateRenderResult {
		bool        ok = false;
		std::string content;
		std::string error;
	};

	[[nodiscard]] auto render_default_config(std::span<const config_schema::Option> options)
	    -> ConfigTemplateRenderResult;

}  // namespace howdy::native::config_template
