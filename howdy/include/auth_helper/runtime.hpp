#pragma once

#include "support/scoped_fd.hpp"

#include <cstdint>
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
	};

	enum class StagedPurgeResult : std::uint8_t {
		kPurged,
		kNothingStaged,
		kInUse,  // an authentication or a staging run holds a slot or the allocation lock
		kFailed,
	};

	enum class StagedPurgeLockWait : std::uint8_t {
		kBrief,  // wait about a second for a staging run to release the allocation lock
		kNone,   // a single attempt, for callers that changed nothing
	};

	auto RuntimeRoot() -> std::filesystem::path;
	auto PrepareRuntimeAuthFiles(const std::string &user, RuntimeIdentity identity)
	    -> std::optional<PreparedPaths>;
	// Removes the staged copies of one user's config and face models. Only root can do this for
	// the production runtime root. Slots in use are kept.
	auto PurgeRuntimeAuthFiles(RuntimeIdentity identity, StagedPurgeLockWait lock_wait)
	    -> StagedPurgeResult;

}  // namespace howdy::native::auth_helper
