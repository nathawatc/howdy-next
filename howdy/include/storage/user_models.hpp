#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <sys/types.h>

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

    struct UserModelEntry {
        int                             id   = -1;
        long long                       time = 0;
        std::string                     label;
        std::string                     backend;
        std::string                     metric;
        std::string                     model;
        std::vector<std::vector<float>> encodings;
    };

    struct NewUserModelEntry {
        std::string                     label;
        std::string                     backend;
        std::string                     metric;
        std::string                     model;
        std::vector<std::vector<float>> encodings;
    };

    struct UserModelListResult {
        UserModelStatus             status = UserModelStatus::kNoModel;
        std::string                 error_message;
        std::vector<UserModelEntry> entries;
        int                         next_id = 0;
    };

    struct UserModelFileSnapshot {
        std::uint64_t  dev            = 0;
        std::uint64_t  inode          = 0;
        std::uintmax_t size           = 0;
        long long      mtime_seconds  = 0;
        long long      mtime_nanosecs = 0;
        long long      ctime_seconds  = 0;
        long long      ctime_nanosecs = 0;
    };

    struct UserModelInspectResult {
        UserModelStatus                      status = UserModelStatus::kNoModel;
        std::string                          error_message;
        std::optional<UserModelFileSnapshot> snapshot;
    };

    struct UserModelEntryExpectation {
        int         id   = -1;
        long long   time = 0;
        std::string label;
        std::string backend;
        std::string metric;
        std::string model;
    };

    struct UserModelMutationResult {
        UserModelStatus status = UserModelStatus::kOk;
        std::string     error_message;
        UserModelEntry  entry;
        bool            removed_last = false;
    };

    struct EncodingModelInfo {
        int         id = -1;
        std::string label;
    };

    struct StoredEncodings {
        std::vector<std::vector<float>> encodings;
        std::vector<EncodingModelInfo>  models;
    };

    struct UserModelLoadResult {
        UserModelStatus status = UserModelStatus::kNoModel;
        std::string     error_message;
        StoredEncodings stored;
    };

    auto load_user_models(const std::string &user, const std::string &expected_backend)
        -> UserModelLoadResult;
    auto load_user_models(const std::string &user, const std::string &expected_backend,
                          std::optional<uid_t> owner_uid) -> UserModelLoadResult;
    auto list_user_model_entries(const std::string &user, const std::string &expected_backend,
                                 const std::string &expected_metric = {},
                                 const std::string &expected_model  = {}) -> UserModelListResult;
    auto inspect_user_model_file(const std::string &user) -> UserModelInspectResult;
    auto append_user_model_entry(const std::string &user, const NewUserModelEntry &entry)
        -> UserModelMutationResult;
    auto remove_user_model_entry(const std::string &user, int id) -> UserModelMutationResult;
    auto remove_user_model_entry_if_matches(const std::string               &user,
                                            const UserModelEntryExpectation &expected)
        -> UserModelMutationResult;
    auto clear_user_model_entries(const std::string &user) -> UserModelMutationResult;
    auto clear_user_model_entries_if_unchanged(const std::string           &user,
                                               const UserModelFileSnapshot &expected_snapshot)
        -> UserModelMutationResult;

}  // namespace howdy::native
