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
        kDirectoryCreateFailed,
        kModelNotFound,
        kModelChanged,
    };

}  // namespace howdy::native
