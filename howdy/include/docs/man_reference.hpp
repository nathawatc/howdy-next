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

		[[nodiscard]] auto ok() const -> bool {
			return error.empty();
		}
	};

	[[nodiscard]] auto render_command_reference(std::span<const native::CommandDescriptor> commands)
	    -> RenderResult;
	[[nodiscard]] auto
	render_global_option_reference(std::span<const native::GlobalOptionDescriptor> options)
	    -> RenderResult;
	[[nodiscard]] auto
	render_workaround_reference(std::span<const pam::WorkaroundDescriptor> workarounds,
	                            pam::Workaround default_mode) -> RenderResult;
	[[nodiscard]] auto
	render_config_option_reference(std::span<const native::config_schema::Option> options)
	    -> RenderResult;

}  // namespace howdy::docs
