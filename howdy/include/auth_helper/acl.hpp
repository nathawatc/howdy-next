#pragma once

#include <filesystem>
#include <unistd.h>

namespace howdy::native::auth_helper {

	__attribute__((visibility("hidden"))) auto
	set_private_acl(int fd, const std::filesystem::path &path, uid_t uid, bool directory) -> bool;

}  // namespace howdy::native::auth_helper
