#pragma once

#include "support/scoped_fd.hpp"

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
		ScopedFd              lease_fd;
	};

	struct RuntimeIdentity {
		uid_t uid = 0;
		gid_t gid = 0;
	};

	auto RuntimeRoot() -> std::filesystem::path;
	auto PrepareRuntimeAuthFiles(const std::string &user, RuntimeIdentity identity)
	    -> std::optional<PreparedPaths>;

}  // namespace howdy::native::auth_helper
