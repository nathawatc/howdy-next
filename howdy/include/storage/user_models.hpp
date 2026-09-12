#pragma once

#include "storage/user_model_status.hpp"
#include "support/file_security/validation_root.hpp"
#include "vision/face_metric.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <sys/types.h>

namespace howdy::native {

	struct UserModelEntry {
		int                             id   = -1;
		long long                       time = 0;
		std::string                     label;
		std::string                     backend;
		std::optional<FaceMetric>       metric;
		std::string                     model;
		std::vector<std::vector<float>> encodings;
	};

	struct NewUserModelEntry {
		std::string                     label;
		std::string                     backend;
		FaceMetric                      metric = FaceMetric::kCosine;
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
		int                       id   = -1;
		long long                 time = 0;
		std::string               label;
		std::string               backend;
		std::optional<FaceMetric> metric;
		std::string               model;
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

	auto LoadUserModels(const std::string &user, const std::string &expected_backend)
	    -> UserModelLoadResult;
	auto LoadUserModels(const std::string &user, const std::string &expected_backend,
	                    std::optional<uid_t>                          owner_uid,
	                    const file_security_internal::ValidationRoot &validation_root = {})
	    -> UserModelLoadResult;
	auto ListUserModelEntries(const std::string &user, const std::string &expected_backend,
	                          std::optional<FaceMetric>                     expected_metric = {},
	                          const std::string                            &expected_model  = {},
	                          const file_security_internal::ValidationRoot &validation_root = {})
	    -> UserModelListResult;
	auto InspectUserModelFile(const std::string                            &user,
	                          const file_security_internal::ValidationRoot &validation_root = {})
	    -> UserModelInspectResult;
	auto AppendUserModelEntry(const std::string &user, const NewUserModelEntry &entry,
	                          const file_security_internal::ValidationRoot &validation_root = {})
	    -> UserModelMutationResult;
	auto RemoveUserModelEntry(const std::string &user, int id,
	                          const file_security_internal::ValidationRoot &validation_root = {})
	    -> UserModelMutationResult;
	auto RemoveUserModelEntryIfMatches(
	    const std::string &user, const UserModelEntryExpectation &expected,
	    const file_security_internal::ValidationRoot &validation_root = {})
	    -> UserModelMutationResult;
	auto ClearUserModelEntries(const std::string                            &user,
	                           const file_security_internal::ValidationRoot &validation_root = {})
	    -> UserModelMutationResult;
	auto ClearUserModelEntriesIfUnchanged(
	    const std::string &user, const UserModelFileSnapshot &expected_snapshot,
	    const file_security_internal::ValidationRoot &validation_root = {})
	    -> UserModelMutationResult;

}  // namespace howdy::native
