#pragma once

#include <cstdint>
#include <string_view>

namespace howdy::native {
	inline constexpr std::string_view kUserModelsDirectoryLabel = "User models directory";
	inline constexpr std::string_view kUserModelFileLabel       = "User model file";
	inline constexpr std::string_view kUserModelLockFileLabel   = "User model lock file";
	inline constexpr std::string_view kUserModelFileInspectionFailedMessage =
	    "Failed to inspect user model file";
	inline constexpr std::string_view kUserModelFileEmptyMessage = "User model file is empty: ";
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
		kModelChanged,
	};

	// True when a mutation that returned `status` may have changed the stored models: it
	// succeeded, or its commit state or durability is uncertain.
	[[nodiscard]] constexpr auto UserModelMutationMayHaveCommitted(UserModelStatus status) noexcept
	    -> bool {
		return status == UserModelStatus::kOk || status == UserModelStatus::kDurabilityUncertain ||
		       status == UserModelStatus::kCommitStateUncertain;
	}

}  // namespace howdy::native
