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

	struct CompareArgsParseResult {
		CompareArgsStatus status    = CompareArgsStatus::kError;
		CompareExit       exit_code = CompareExit::kAbort;
		std::string       message;
		CompareArgs       args;
	};

	// Compare takes one argument, the user; its config and model come as descriptors
	// (protocol/compare_descriptors.hpp).
	[[nodiscard]] auto ParseCompareArgs(int argc, char **argv) -> CompareArgsParseResult;

}  // namespace howdy::native
