#pragma once

#include "app/command_catalog.hpp"
#include "app/command_invocation.hpp"
#include "support/invoking_user.hpp"

#include <array>

#include <sys/types.h>

namespace howdy::native::howdy_internal {

	using CommandMain  = int (*)(const CommandInvocation &invocation);
	using CommandMains = std::array<CommandMain, static_cast<std::size_t>(CommandId::kCount)>;

	struct HowdyDependencies {
		void *context                                                      = nullptr;
		InvokingIdentityResult (*resolve_invoking_identity)(void *context) = nullptr;
		uid_t (*effective_uid)(void *context)                              = nullptr;
		CommandMains command_mains{};
	};

	[[nodiscard]] auto ProductionCommandMains() -> CommandMains;
	auto HowdyMainWithDependencies(int argc, char **argv, const HowdyDependencies &dependencies)
	    -> int;

}  // namespace howdy::native::howdy_internal
