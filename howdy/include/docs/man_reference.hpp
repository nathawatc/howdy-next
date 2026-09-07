#pragma once

#include "app/command_catalog.hpp"
#include "config/config_schema.hpp"
#include "module/pam_option_catalog.hpp"

#include <span>
#include <string>

namespace howdy::docs {

	struct RenderResult {
		std::string output;
		std::string error;

		[[nodiscard]] auto Ok() const -> bool {
			return error.empty();
		}
	};

	[[nodiscard]] auto RenderCommandReference(std::span<const native::CommandDescriptor> commands)
	    -> RenderResult;
	[[nodiscard]] auto
	RenderGlobalOptionReference(std::span<const native::GlobalOptionDescriptor> options)
	    -> RenderResult;
	[[nodiscard]] auto
	RenderWorkaroundReference(std::span<const pam::WorkaroundDescriptor> workarounds,
	                          pam::Workaround default_mode) -> RenderResult;
	[[nodiscard]] auto
	RenderConfigOptionReference(std::span<const native::config_schema::Option> options)
	    -> RenderResult;

}  // namespace howdy::docs
