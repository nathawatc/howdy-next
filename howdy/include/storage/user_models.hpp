#pragma once

#include "storage/user_model_types.hpp"
#include "support/file_security/validation_root.hpp"

#include <filesystem>
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
	// Loads from a descriptor opened by another process, never through a path. It must be open
	// read-only and name a regular file with one link, owned by `owner_uid`, that neither group
	// nor others can write. The read is positional, so the shared file offset does not move.
	// `display_path` only names the source in messages.
	auto LoadUserModelsFromFd(int model_fd, const std::filesystem::path &display_path,
	                          const std::string &expected_backend, uid_t owner_uid)
	    -> UserModelLoadResult;
	auto ListUserModelEntries(const std::string &user, const std::string &expected_backend,
	                          const std::string                            &expected_model  = {},
	                          const file_security_internal::ValidationRoot &validation_root = {})
	    -> UserModelListResult;
	auto InspectUserModelFile(const std::string                            &user,
	                          const file_security_internal::ValidationRoot &validation_root = {})
	    -> UserModelInspectResult;
	auto AppendUserModelEntry(const std::string &user, const NewUserModelEntry &entry,
	                          const file_security_internal::ValidationRoot &validation_root = {})
	    -> UserModelMutationResult;
	auto RemoveUserModelEntryIfMatches(
	    const std::string &user, const UserModelEntryExpectation &expected,
	    const file_security_internal::ValidationRoot &validation_root = {})
	    -> UserModelMutationResult;
	auto ClearUserModelEntriesIfUnchanged(
	    const std::string &user, const UserModelFileSnapshot &expected_snapshot,
	    const file_security_internal::ValidationRoot &validation_root = {})
	    -> UserModelMutationResult;

}  // namespace howdy::native
