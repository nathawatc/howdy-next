#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <sys/types.h>

namespace howdy::native {

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

	[[nodiscard]] auto check_opencv_model_readiness_with_label(
	    const std::filesystem::path &path, std::string_view label,
	    const std::optional<uid_t> &owner_uid) -> OpenCvModelReadiness;

}  // namespace howdy::native
