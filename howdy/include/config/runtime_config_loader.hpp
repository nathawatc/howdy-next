#pragma once

#include "config/runtime_config.hpp"
#include "support/file_security/validation_root.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include <sys/types.h>

namespace howdy::native {

	enum class RuntimeConfigLoadStatus : std::uint8_t {
		kOk,
		kPathError,
		kParseError,
		kInvalidRuntimeValue,
	};

	struct RuntimeConfigLoadResult {
		bool                         ok     = false;
		RuntimeConfigLoadStatus      status = RuntimeConfigLoadStatus::kPathError;
		std::filesystem::path        path;
		std::optional<RuntimeConfig> config;
		std::string                  error_message;
		int                          error_code = 0;
	};

	auto LoadRuntimeConfig(const std::filesystem::path &config_path, std::optional<uid_t> owner_uid,
	                       const file_security_internal::ValidationRoot &validation_root = {})
	    -> RuntimeConfigLoadResult;
#ifndef HOWDY_RUNTIME_CONFIG_EXPLICIT_PATH_ONLY
	auto LoadRuntimeConfig(const std::filesystem::path &config_path) -> RuntimeConfigLoadResult;
	auto LoadRuntimeConfig() -> RuntimeConfigLoadResult;
#endif

}  // namespace howdy::native
