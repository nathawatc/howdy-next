#pragma once

#include "auth_helper/auth_helper_acl_policy.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <optional>
#include <unistd.h>

#include <sys/types.h>

#include <acl/libacl.h>

namespace howdy::test::auth_helper {

	constexpr acl_perm_t kAclRead    = acl_perm_t{ACL_READ};
	constexpr acl_perm_t kAclWrite   = acl_perm_t{ACL_WRITE};
	constexpr acl_perm_t kAclExecute = acl_perm_t{ACL_EXECUTE};

	enum class AclSupport : std::uint8_t {
		kSupported,
		kUnsupported,
		kError,
	};

	inline auto add_acl_probe_entry(acl_t &acl, acl_tag_t tag, const void *qualifier,
	                                acl_perm_t permissions) -> bool {
		acl_entry_t   entry;
		acl_permset_t permission_set;
		if (acl_create_entry(&acl, &entry) != 0 || acl_set_tag_type(entry, tag) != 0 ||
		    (qualifier != nullptr && acl_set_qualifier(entry, qualifier) != 0) ||
		    acl_get_permset(entry, &permission_set) != 0 || acl_clear_perms(permission_set) != 0) {
			return false;
		}
		for (const acl_perm_t permission : {kAclRead, kAclWrite, kAclExecute}) {
			if ((permissions & permission) != 0 && acl_add_perm(permission_set, permission) != 0) {
				return false;
			}
		}
		return acl_set_permset(entry, permission_set) == 0;
	}

	struct AclProbeResult {
		bool ready           = false;
		bool policy_mismatch = false;
		int  error_number    = 0;
	};

	inline auto configure_acl_probe(acl_t &acl, int fd, const std::filesystem::path &probe_path,
	                                std::optional<int> injected_error) -> AclProbeResult {
		const uid_t uid          = geteuid();
		bool        ready        = add_acl_probe_entry(acl, ACL_USER_OBJ, nullptr, kAclRead) &&
		                           add_acl_probe_entry(acl, ACL_USER, &uid, kAclRead) &&
		                           add_acl_probe_entry(acl, ACL_GROUP_OBJ, nullptr, 0) &&
		                           add_acl_probe_entry(acl, ACL_MASK, nullptr, kAclRead) &&
		                           add_acl_probe_entry(acl, ACL_OTHER, nullptr, 0);
		int         error_number = ready ? 0 : errno;
		if (ready && acl_valid(acl) != 0) {
			error_number = errno;
			ready        = false;
		}
		if (ready && injected_error.has_value()) {
			errno        = *injected_error;
			error_number = errno;
			ready        = false;
		}
		if (ready && acl_set_fd(fd, acl) != 0) {
			error_number = errno;
			ready        = false;
		}
		if (!ready) {
			return {.error_number = error_number};
		}
		const auto acl_check = check_private_acl(probe_path, uid, false);
		if (acl_check.status == AclCheckStatus::kError) {
			return {.error_number = acl_check.error_number};
		}
		if (acl_check.status == AclCheckStatus::kMismatch) {
			std::cerr << "FAIL: ACL support probe policy mismatch '" << probe_path << "'\n";
			return {.policy_mismatch = true};
		}
		return {.ready = true};
	}

	inline auto acl_support(const std::filesystem::path &root,
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

		AclProbeResult probe_result;
		AclSupport     result = AclSupport::kError;
		acl_t          acl    = acl_init(5);
		if (acl == nullptr) {
			probe_result.error_number = errno;
		} else {
			probe_result = configure_acl_probe(acl, fd, probe_path, injected_error);
			if (probe_result.ready) {
				result = AclSupport::kSupported;
			}
		}
		if (acl != nullptr) {
			acl_free(acl);
		}
		close(fd);
		std::error_code cleanup_ec;
		std::filesystem::remove(probe_path, cleanup_ec);
		if (result == AclSupport::kSupported) {
			return result;
		}
		if (probe_result.policy_mismatch) {
			return AclSupport::kError;
		}
		if (probe_result.error_number == ENOTSUP || probe_result.error_number == EOPNOTSUPP) {
			return AclSupport::kUnsupported;
		}
		std::cerr << "FAIL: ACL support probe '" << probe_path
		          << "': " << std::strerror(probe_result.error_number) << "\n";
		return AclSupport::kError;
	}

}  // namespace howdy::test::auth_helper
