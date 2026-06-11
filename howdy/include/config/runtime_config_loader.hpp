#pragma once

#include "config/config_reader.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <sys/types.h>

namespace howdy::native {

	enum class RuntimeConfigLoadStatus {
		kOk,
		kPathError,
		kParseError,
		kInvalidRuntimeValue,
	};

	struct RuntimeConfigLoadResult {
		RuntimeConfigLoadStatus       status = RuntimeConfigLoadStatus::kPathError;
		std::filesystem::path         path;
		std::unique_ptr<ConfigReader> config;
		std::string                   error_message;
		int                           error_code = 0;
	};

	auto load_runtime_config(const std::filesystem::path &path, std::optional<uid_t> owner_uid)
	    -> RuntimeConfigLoadResult;
	auto load_runtime_config(const std::filesystem::path &path) -> RuntimeConfigLoadResult;
	auto load_runtime_config() -> RuntimeConfigLoadResult;

}  // namespace howdy::native
