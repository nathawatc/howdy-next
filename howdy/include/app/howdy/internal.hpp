#pragma once

#include "app/command_catalog.hpp"

#include <array>
#include <string>

#include <sys/types.h>

namespace howdy::native::howdy_internal {

	using CommandMain = int (*)(int argc, char **argv);

	struct HowdyDependencies {
		void *context                              = nullptr;
		std::string (*resolve_user)(void *context) = nullptr;
		uid_t (*effective_uid)(void *context)      = nullptr;
		std::array<CommandMain, static_cast<std::size_t>(CommandId::kCount)> command_mains{};
	};

	auto HowdyMainWithDependencies(int argc, char **argv, const HowdyDependencies &dependencies)
	    -> int;

}  // namespace howdy::native::howdy_internal
