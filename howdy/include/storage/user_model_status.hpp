#pragma once

#include <cstdint>
#include <string_view>

namespace howdy::native {
	inline constexpr std::string_view kUserModelsDirectoryLabel = "User models directory";
	inline constexpr std::string_view kUserModelFileLabel       = "User model file";
	inline constexpr std::string_view kUserModelFileInspectionFailedMessage =
	    "Failed to inspect user model file";
	inline constexpr std::string_view kUserModelChangedMessage =
	    "User model file changed, please rerun the command";
	inline constexpr std::string_view kStoredEncodingsLimitMessage =
	    "Stored face model contains too many encodings";
	inline constexpr std::string_view kStoredModelListLimitMessage =
	    "Stored face model list exceeds safety limit";
	inline constexpr std::string_view kStoredModelIdLimitMessage =
	    "Stored face model ID is too large";

	enum class UserModelStatus : std::uint8_t {
		kOk,
		kNoModel,
		kNoModelDirectory,
		kIncompatibleBackend,
		kIncompatibleMetric,
		kIncompatibleModel,
		kParseError,
		kInvalidShape,
		kOversized,
		kInsecurePath,
		kInvalidUser,
		kLockFailed,
		kWriteFailed,
		kAtomicExchangeUnsupported,
		kDeleteFailed,
		kDurabilityUncertain,
		kCommitStateUncertain,
		kDirectoryCreateFailed,
		kModelNotFound,
		kModelChanged,
	};

}  // namespace howdy::native
