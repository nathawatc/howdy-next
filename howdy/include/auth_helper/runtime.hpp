#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unistd.h>

#include <sys/types.h>

namespace howdy::native::auth_helper {

	struct PreparedPaths {
		std::filesystem::path runtime_dir;
		std::filesystem::path config_path;
		std::filesystem::path user_models_dir;
		int                   lease_fd = -1;
	};


	struct RuntimeIdentity {
		uid_t uid = 0;
		gid_t gid = 0;
	};

	auto runtime_root() -> std::filesystem::path;
	auto prepare_runtime_auth_files(const std::string &user, RuntimeIdentity identity)
	    -> std::optional<PreparedPaths>;

}  // namespace howdy::native::auth_helper
