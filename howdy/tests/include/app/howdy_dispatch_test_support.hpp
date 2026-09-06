#pragma once

#include "app/command_catalog.hpp"
#include "app/howdy/internal.hpp"

#include <array>
#include <optional>
#include <string>
#include <vector>

#include <sys/types.h>

namespace howdy::test::dispatch {

	using CommandMain = howdy::native::howdy_internal::CommandMain;
	using CommandMains =
	    std::array<CommandMain, static_cast<std::size_t>(howdy::native::CommandId::kCount)>;

	struct Context {
		std::string                             resolved_user = "alice";
		uid_t                                   effective_uid = 0;
		std::vector<std::string>                command_arguments;
		int                                     command_result = 0;
		std::optional<howdy::native::CommandId> command_id;
		int                                     resolve_user_calls  = 0;
		int                                     effective_uid_calls = 0;
	};

	struct RunResult {
		int         status;
		std::string output;
		std::string error;
	};

	auto run(Context &context, std::vector<std::string> arguments) -> RunResult;
	auto run(Context &context, std::vector<std::string> arguments, CommandMain list_callback)
	    -> RunResult;
	auto run(Context &context, std::vector<std::string> arguments, CommandMain list_callback,
	         CommandMains command_mains) -> RunResult;
	auto run_howdy_completion_tests() -> bool;

}  // namespace howdy::test::dispatch
