#pragma once

#include "common/compare_exit.hpp"

#include <cstdint>
#include <string>

namespace howdy::native {

	struct CompareArgs {
		std::string user;
		std::string config_path;
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

	auto parse_compare_args(int argc, char **argv, const std::string &default_config_path)
	    -> CompareArgsParseResult;

}  // namespace howdy::native
