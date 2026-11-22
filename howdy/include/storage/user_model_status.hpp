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

	enum class UserModelOutcome : std::uint8_t {
		kOk,
		kAbsent,
		kChanged,
		kInsecure,
		kFailedUncommitted,
		kCommittedNotDurable,
		kUncertain,
	};

	[[nodiscard]] constexpr auto Classify(UserModelStatus status) -> UserModelOutcome {
		switch (status) {
			case UserModelStatus::kOk:
				return UserModelOutcome::kOk;
			case UserModelStatus::kNoModel:
			case UserModelStatus::kNoModelDirectory:
				return UserModelOutcome::kAbsent;
			case UserModelStatus::kModelChanged:
				return UserModelOutcome::kChanged;
			case UserModelStatus::kInsecurePath:
				return UserModelOutcome::kInsecure;
			case UserModelStatus::kDurabilityUncertain:
				return UserModelOutcome::kCommittedNotDurable;
			case UserModelStatus::kCommitStateUncertain:
				return UserModelOutcome::kUncertain;
			case UserModelStatus::kIncompatibleBackend:
			case UserModelStatus::kIncompatibleModel:
			case UserModelStatus::kParseError:
			case UserModelStatus::kInvalidShape:
			case UserModelStatus::kOversized:
			case UserModelStatus::kInvalidUser:
			case UserModelStatus::kLockFailed:
			case UserModelStatus::kWriteFailed:
			case UserModelStatus::kAtomicExchangeUnsupported:
			case UserModelStatus::kDeleteFailed:
			case UserModelStatus::kDirectoryCreateFailed:
				return UserModelOutcome::kFailedUncommitted;
		}
		return UserModelOutcome::kUncertain;
	}

}  // namespace howdy::native
