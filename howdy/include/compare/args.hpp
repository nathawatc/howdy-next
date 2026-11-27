#pragma once

#include "protocol/compare_exit.hpp"

#include <cstdint>
#include <string>

namespace howdy::native {
	struct CompareArgs {
		std::string user;
	};

	enum class CompareArgsStatus : std::uint8_t {
		kOk,
		kHelp,
		kError,
	};

	enum class CompareArgsMode : std::uint8_t {
		kStandalone,
		kProtocol,
	};

	struct CompareArgsParseResult {
		CompareArgsStatus status    = CompareArgsStatus::kError;
		CompareExit       exit_code = CompareExit::kAbort;
		std::string       message;
		CompareArgs       args;
	};

	// Compare takes one argument, the user; its config and model come as descriptors
	// (protocol/compare_descriptors.hpp). In kProtocol mode, argv[1] is always treated as the
	// user position and never interpreted as interactive --help/-h.
	[[nodiscard]] auto ParseCompareArgs(int argc, char **argv,
	                                    CompareArgsMode mode = CompareArgsMode::kStandalone)
	    -> CompareArgsParseResult;

}  // namespace howdy::native
