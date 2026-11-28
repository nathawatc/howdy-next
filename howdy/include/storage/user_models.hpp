#pragma once

#include "storage/user_model_types.hpp"

#include <filesystem>
#include <string>

#include <sys/types.h>

namespace howdy::native {

	// Loads from a descriptor opened by another process, never through a path. It must be open
	// read-only and name a regular file with one link, owned by `owner_uid`, that neither group
	// nor others can write. The read is positional, so the shared file offset does not move.
	// `display_path` only names the source in messages.
	[[nodiscard]] auto LoadUserModelsFromFd(int model_fd, const std::filesystem::path &display_path,
	                                        const std::string &expected_backend,
	                                        const std::string &expected_model, uid_t owner_uid)
	    -> UserModelLoadResult;

}  // namespace howdy::native
