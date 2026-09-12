#pragma once

#include "support/file_security/validation_root.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <sys/types.h>

namespace howdy::native {
	inline constexpr auto kModelsDirectoryLabel = "Models directory";

	enum class OpenCvModelStatus : std::uint8_t {
		kOk,
		kMissing,
		kInsecure,
		kInvalid,
	};

	struct OpenCvModelReadiness {
		OpenCvModelStatus status = OpenCvModelStatus::kInsecure;
		std::string       error_message;
	};

	[[nodiscard]] auto CheckOpencvModelReadinessWithLabel(
	    const std::filesystem::path &path, std::string_view label,
	    const std::optional<uid_t>                   &owner_uid,
	    const file_security_internal::ValidationRoot &validation_root = {}) -> OpenCvModelReadiness;

}  // namespace howdy::native
