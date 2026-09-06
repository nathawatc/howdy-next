#pragma once

#include "auth_helper/acl.hpp"
#include "auth_helper/runtime.hpp"

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
	validate_runtime_root(const std::filesystem::path &path, uid_t owner_uid, gid_t owner_gid)
	    -> bool;
	__attribute__((visibility("hidden"))) auto
	secure_source_file_stat(int fd, const std::string &label, uid_t owner_uid) -> bool;
	__attribute__((visibility("hidden"))) auto
	select_source_model_path(const std::filesystem::path &source_user_models_dir,
	                         const std::string &user, std::optional<uid_t> owner_uid,
	                         std::optional<std::filesystem::path> &source_model_path) -> bool;
	__attribute__((visibility("hidden"))) auto
	prepare_runtime_auth_files(const std::string &user, StagedIdentity identity,
	                           const RuntimeSources &sources, const AclOperations &operations)
	    -> std::optional<PreparedPaths>;
	__attribute__((visibility("hidden"))) auto
	cleanup_runtime_auth_files(const std::filesystem::path &path, uid_t uid,
	                           const std::filesystem::path &runtime_root, uid_t owner_uid = 0,
	                           gid_t owner_gid = 0) -> CleanupRuntimeResult;
}  // namespace howdy::native::auth_helper::internal
