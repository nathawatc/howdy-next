#pragma once

#include "auth_helper/acl.hpp"

#include <cerrno>
#include <map>
#include <utility>

#include <sys/stat.h>
#include <sys/types.h>

#include <acl/libacl.h>

namespace howdy::test::auth_helper {

	struct FakeAclContext {
		using FileIdentity = std::pair<dev_t, ino_t>;

		std::map<FileIdentity, acl_t> acls_by_identity;

		~FakeAclContext() {
			Clear();
		}

		void Clear() {
			for (const auto &entry : acls_by_identity) {
				acl_free(entry.second);
			}
			acls_by_identity.clear();
		}

		[[nodiscard]] auto Operations() -> howdy::native::auth_helper::AclOperations;
	};

	inline auto FakeAclSetFd(void *context, int fd, acl_t acl) -> int {
		auto       &state = *static_cast<FakeAclContext *>(context);
		struct stat status{};
		if (fstat(fd, &status) != 0) {
			return -1;
		}
		acl_t duplicate = acl_dup(acl);
		if (duplicate == nullptr) {
			return -1;
		}
		const FakeAclContext::FileIdentity identity{status.st_dev, status.st_ino};
		if (const auto existing = state.acls_by_identity.find(identity);
		    existing != state.acls_by_identity.end()) {
			acl_free(existing->second);
			existing->second = duplicate;
		} else {
			state.acls_by_identity.emplace(identity, duplicate);
		}
		return 0;
	}

	inline auto FakeAclGetFd(void *context, int fd) -> acl_t {
		auto       &state = *static_cast<FakeAclContext *>(context);
		struct stat status{};
		if (fstat(fd, &status) != 0) {
			return nullptr;
		}
		const FakeAclContext::FileIdentity identity{status.st_dev, status.st_ino};
		const auto                         stored = state.acls_by_identity.find(identity);
		if (stored == state.acls_by_identity.end()) {
			errno = ENODATA;
			return nullptr;
		}
		return acl_dup(stored->second);
	}

	inline auto FakeAclContext::Operations() -> howdy::native::auth_helper::AclOperations {
		return {.context = this, .acl_get_fd = FakeAclGetFd, .acl_set_fd = FakeAclSetFd};
	}

}  // namespace howdy::test::auth_helper
