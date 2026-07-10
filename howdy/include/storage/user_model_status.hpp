#pragma once

namespace howdy::native {

	enum class UserModelStatus {
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
		kDeleteFailed,
		kDurabilityUncertain,
		kCommitStateUncertain,
		kDirectoryCreateFailed,
		kModelNotFound,
		kModelChanged,
	};

}  // namespace howdy::native
