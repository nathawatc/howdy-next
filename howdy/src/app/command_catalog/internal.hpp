#pragma once

#include "app/command_catalog.hpp"

#include <optional>
#include <span>
#include <string>

namespace howdy::native::command_catalog_internal {

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	ValidateCommands(std::span<const CommandDescriptor> commands, bool require_complete)
	    -> std::optional<std::string>;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	ValidateGlobalOptions(std::span<const GlobalOptionDescriptor> options, bool require_complete)
	    -> std::optional<std::string>;

}  // namespace howdy::native::command_catalog_internal
