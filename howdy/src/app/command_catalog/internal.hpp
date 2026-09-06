#pragma once

#include "app/command_catalog.hpp"

#include <optional>
#include <span>
#include <string>

namespace howdy::native::command_catalog_internal {

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	validate_commands(std::span<const CommandDescriptor> commands, bool require_complete)
	    -> std::optional<std::string>;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	validate_global_options(std::span<const GlobalOptionDescriptor> options, bool require_complete)
	    -> std::optional<std::string>;

}  // namespace howdy::native::command_catalog_internal
