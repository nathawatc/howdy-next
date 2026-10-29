#pragma once

#include "auth_helper/acl.hpp"
#include "storage/staged_runtime_policy.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

#include <sys/acl.h>
#include <sys/types.h>

namespace howdy::test::auth_helper {

	enum class AclSupport : std::uint8_t {
		kSupported,
		kUnsupported,
		kError,
	};

	struct AclProbeContext {
		std::optional<int> injected_error;
		int                error_number = 0;
	};

	inline auto ProbeAclSetFd(void *raw_context, int fd, acl_t acl) -> int {
		auto &context = *static_cast<AclProbeContext *>(raw_context);
		if (context.injected_error.has_value()) {
			errno                = *context.injected_error;
			context.error_number = *context.injected_error;
			return -1;
		}
		if (acl_set_fd(fd, acl) != 0) {
			context.error_number = errno;
			return -1;
		}
		return 0;
	}

	inline auto ProbeAclGetFd(void *raw_context, int fd) -> acl_t {
		auto &context = *static_cast<AclProbeContext *>(raw_context);
		acl_t acl     = acl_get_fd(fd);
		if (acl == nullptr) {
			context.error_number = errno;
		}
		return acl;
	}

	inline auto ProbeAclSupport(const std::filesystem::path &root,
	                            std::optional<int> injected_error = std::nullopt) -> AclSupport {
		const auto probe_path = root / "acl-support-probe";
		const int  fd =
		    open(probe_path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
		if (fd < 0) {
			const int error_number = errno;
			std::cerr << "FAIL: open ACL support probe '" << probe_path
			          << "': " << std::strerror(error_number) << "\n";
			return AclSupport::kError;
		}

		AclProbeContext                                 context{.injected_error = injected_error};
		const howdy::native::auth_helper::AclOperations operations{
		    .context    = &context,
		    .acl_get_fd = ProbeAclGetFd,
		    .acl_set_fd = ProbeAclSetFd,
		};
		const auto policy =
		    howdy::native::GetStagedRuntimePolicy(howdy::native::StagedRuntimeRole::kConfig).acl;
		const bool ready = howdy::native::auth_helper::SetPersistentAclWithOperations(
		    fd, probe_path, geteuid(), policy, operations);
		close(fd);
		std::error_code cleanup_ec;
		std::filesystem::remove(probe_path, cleanup_ec);
		if (ready) {
			return AclSupport::kSupported;
		}
		if (context.error_number == ENOTSUP || context.error_number == EOPNOTSUPP) {
			return AclSupport::kUnsupported;
		}
		return AclSupport::kError;
	}

}  // namespace howdy::test::auth_helper
