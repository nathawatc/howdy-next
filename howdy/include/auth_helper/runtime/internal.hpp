#pragma once

#include "auth_helper/acl.hpp"
#include "auth_helper/runtime.hpp"
#include "support/file_security/validation_root.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <unistd.h>

namespace howdy::native::auth_helper::internal {
	struct StagedIdentity {
		uid_t target_uid = 0;
		uid_t owner_uid  = 0;
		gid_t owner_gid  = 0;
	};

	struct RuntimeSources {
		std::filesystem::path runtime_root;
		std::filesystem::path config;
		std::filesystem::path user_models_dir;
	};

	__attribute__((visibility("hidden"))) auto
	ValidateRuntimeRoot(const std::filesystem::path &path, uid_t owner_uid, gid_t owner_gid)
	    -> bool;
	__attribute__((visibility("hidden"))) auto
	SecureSourceFileStat(int fd, const std::string &label, uid_t owner_uid) -> bool;
	__attribute__((visibility("hidden"))) auto SelectSourceModelPath(
	    const std::filesystem::path &source_user_models_dir, const std::string &user,
	    std::optional<uid_t> owner_uid, std::optional<std::filesystem::path> &source_model_path,
	    const file_security_internal::ValidationRoot &validation_root = {}) -> bool;
	__attribute__((visibility("hidden"))) auto
	PrepareRuntimeAuthFiles(const std::string &user, StagedIdentity identity,
	                        const RuntimeSources &sources, const AclOperations &operations)
	    -> std::optional<PreparedPaths>;
}  // namespace howdy::native::auth_helper::internal
