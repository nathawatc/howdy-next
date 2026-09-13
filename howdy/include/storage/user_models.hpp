#pragma once

#include "storage/user_model_types.hpp"
#include "support/file_security/validation_root.hpp"

#include <optional>
#include <string>

#include <sys/types.h>

namespace howdy::native {

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
