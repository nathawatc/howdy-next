#include "auth_helper_acl.hpp"

#include "auth_helper_runtime.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

#include <acl/libacl.h>

namespace howdy::native::auth_helper {
	namespace {
		constexpr acl_perm_t kAclRead    = acl_perm_t{ACL_READ};
		constexpr acl_perm_t kAclWrite   = acl_perm_t{ACL_WRITE};
		constexpr acl_perm_t kAclExecute = acl_perm_t{ACL_EXECUTE};

#ifdef HOWDY_AUTH_HELPER_TESTING
		bool            g_fail_acl_setup        = false;
		bool            g_fail_acl_verification = false;
		AclSetFdForTest g_acl_set_fd_for_test   = nullptr;
		AclGetFdForTest g_acl_get_fd_for_test   = nullptr;
		AclResetForTest g_acl_reset_for_test    = nullptr;
#endif

		auto log_errno_failure(std::string_view operation, const std::filesystem::path &path,
		                       int error_number) -> bool {
			std::cerr << "Failed to " << operation << " '" << path
			          << "': " << std::strerror(error_number) << "\n";
			return false;
		}

		enum class AclVerifyStatus : std::uint8_t {
			kOk,
			kReadError,
			kIterationError,
			kPermissionQueryError,
			kMalformed,
		};

		struct AclVerifyResult {
			AclVerifyStatus  status;
			int              error_number = 0;
			std::string_view operation;
		};

		auto acl_entry_has_permissions(acl_entry_t entry, acl_tag_t tag, const void *qualifier,
		                               int permissions) -> AclVerifyResult {
			acl_tag_t entry_tag;
			if (acl_get_tag_type(entry, &entry_tag) != 0) {
				return {.status       = AclVerifyStatus::kReadError,
				        .error_number = errno,
				        .operation    = "acl_get_tag_type"};
			}
			if (entry_tag != tag) {
				return {.status = AclVerifyStatus::kMalformed};
			}
			if (qualifier != nullptr) {
				void *entry_qualifier = acl_get_qualifier(entry);
				if (entry_qualifier == nullptr) {
					return {.status       = AclVerifyStatus::kReadError,
					        .error_number = errno,
					        .operation    = "acl_get_qualifier"};
				}
				const bool matches = std::memcmp(entry_qualifier, qualifier, sizeof(uid_t)) == 0;
				acl_free(entry_qualifier);
				if (!matches) {
					return {.status = AclVerifyStatus::kMalformed};
				}
			}

			acl_permset_t permission_set;
			if (acl_get_permset(entry, &permission_set) != 0) {
				return {.status       = AclVerifyStatus::kPermissionQueryError,
				        .error_number = errno,
				        .operation    = "acl_get_permset"};
			}
			const int read_result = acl_get_perm(permission_set, ACL_READ);
			if (read_result < 0) {
				return {.status       = AclVerifyStatus::kPermissionQueryError,
				        .error_number = errno,
				        .operation    = "acl_get_perm"};
			}
			const int write_result = acl_get_perm(permission_set, ACL_WRITE);
			if (write_result < 0) {
				return {.status       = AclVerifyStatus::kPermissionQueryError,
				        .error_number = errno,
				        .operation    = "acl_get_perm"};
			}
			const int execute_result = acl_get_perm(permission_set, ACL_EXECUTE);
			if (execute_result < 0) {
				return {.status       = AclVerifyStatus::kPermissionQueryError,
				        .error_number = errno,
				        .operation    = "acl_get_perm"};
			}
			if ((read_result == 1) != ((permissions & ACL_READ) != 0) ||
			    (write_result == 1) != ((permissions & ACL_WRITE) != 0) ||
			    (execute_result == 1) != ((permissions & ACL_EXECUTE) != 0)) {
				return {.status = AclVerifyStatus::kMalformed};
			}
			return {.status = AclVerifyStatus::kOk};
		}

		auto get_fd_acl(int fd) -> acl_t {
#ifdef HOWDY_AUTH_HELPER_TESTING
			if (g_acl_get_fd_for_test != nullptr) {
				return g_acl_get_fd_for_test(fd);
			}
#endif
			return acl_get_fd(fd);
		}

		auto set_fd_acl(int fd, acl_t acl) -> int {
#ifdef HOWDY_AUTH_HELPER_TESTING
			if (g_acl_set_fd_for_test != nullptr) {
				return g_acl_set_fd_for_test(fd, acl);
			}
#endif
			return acl_set_fd(fd, acl);
		}

		auto verify_private_acl(int fd, bool directory, uid_t uid) -> AclVerifyResult {
#ifdef HOWDY_AUTH_HELPER_TESTING
			if (g_fail_acl_verification) {
				return {.status = AclVerifyStatus::kMalformed};
			}
#endif
			acl_t acl = get_fd_acl(fd);
			if (acl == nullptr) {
				return {.status       = AclVerifyStatus::kReadError,
				        .error_number = errno,
				        .operation    = "acl_get_fd"};
			}

			const int            permissions   = directory ? (ACL_READ | ACL_EXECUTE) : ACL_READ;
			constexpr std::array kExpectedTags = {ACL_USER_OBJ, ACL_USER, ACL_GROUP_OBJ, ACL_MASK,
			                                      ACL_OTHER};
			std::size_t          entry_count   = 0;
			acl_entry_t          entry;
			int                  entry_id = ACL_FIRST_ENTRY;
			while (true) {
				const int entry_result = acl_get_entry(acl, entry_id, &entry);
				if (entry_result < 0) {
					const int error_number = errno;
					acl_free(acl);
					return {.status       = AclVerifyStatus::kIterationError,
					        .error_number = error_number,
					        .operation    = "acl_get_entry"};
				}
				if (entry_result == 0) {
					break;
				}
				entry_id = ACL_NEXT_ENTRY;
				if (entry_count >= kExpectedTags.size()) {
					acl_free(acl);
					return {.status = AclVerifyStatus::kMalformed};
				}
				acl_tag_t tag;
				if (acl_get_tag_type(entry, &tag) != 0) {
					const int error_number = errno;
					acl_free(acl);
					return {.status       = AclVerifyStatus::kReadError,
					        .error_number = error_number,
					        .operation    = "acl_get_tag_type"};
				}
				if (tag != kExpectedTags[entry_count]) {
					acl_free(acl);
					return {.status = AclVerifyStatus::kMalformed};
				}
				++entry_count;
				const auto result = acl_entry_has_permissions(
				    entry, tag, tag == ACL_USER ? static_cast<const void *>(&uid) : nullptr,
				    tag == ACL_GROUP_OBJ || tag == ACL_OTHER ? 0 : permissions);
				if (result.status != AclVerifyStatus::kOk) {
					acl_free(acl);
					return result;
				}
			}
			acl_free(acl);
			if (entry_count != kExpectedTags.size()) {
				return {.status = AclVerifyStatus::kMalformed};
			}
			return {.status = AclVerifyStatus::kOk};
		}

		auto log_acl_verify_failure(const std::filesystem::path &path,
		                            const AclVerifyResult       &result) -> bool {
			if (result.status == AclVerifyStatus::kMalformed) {
				std::cerr << "ACL policy mismatch for staged object '" << path << "'\n";
				return false;
			}
			return log_errno_failure(std::string(result.operation) + " while verifying ACL", path,
			                         result.error_number);
		}

		auto add_acl_entry(acl_t &acl, const std::filesystem::path &path, acl_tag_t tag,
		                   const void *qualifier, acl_perm_t entry_permissions) -> bool {
			acl_entry_t   entry;
			acl_permset_t permission_set;
			if (acl_create_entry(&acl, &entry) != 0) {
				return log_errno_failure("acl_create_entry for staged object", path, errno);
			}
			if (acl_set_tag_type(entry, tag) != 0) {
				return log_errno_failure("acl_set_tag_type for staged object", path, errno);
			}
			if (qualifier != nullptr && acl_set_qualifier(entry, qualifier) != 0) {
				return log_errno_failure("acl_set_qualifier for staged object", path, errno);
			}
			if (acl_get_permset(entry, &permission_set) != 0) {
				return log_errno_failure("acl_get_permset for staged object", path, errno);
			}
			if (acl_clear_perms(permission_set) != 0) {
				return log_errno_failure("acl_clear_perms for staged object", path, errno);
			}
			for (const acl_perm_t permission : {kAclRead, kAclWrite, kAclExecute}) {
				if ((entry_permissions & permission) != 0 &&
				    acl_add_perm(permission_set, permission) != 0) {
					return log_errno_failure("acl_add_perm for staged object", path, errno);
				}
			}
			if (acl_set_permset(entry, permission_set) != 0) {
				return log_errno_failure("acl_set_permset for staged object", path, errno);
			}
			return true;
		}

	}  // namespace

	auto set_private_acl(int fd, const std::filesystem::path &path, uid_t uid, bool directory)
	    -> bool {
#ifdef HOWDY_AUTH_HELPER_TESTING
		if (g_fail_acl_setup) {
			errno                  = EIO;
			const int error_number = errno;
			return log_errno_failure("apply ACL to staged object", path, error_number);
		}
#endif
		const acl_perm_t permissions = directory ? (kAclRead | kAclExecute) : kAclRead;
		acl_t            acl         = acl_init(5);
		if (acl == nullptr) {
			const int error_number = errno;
			return log_errno_failure("allocate ACL for staged object", path, error_number);
		}

		if (!add_acl_entry(acl, path, ACL_USER_OBJ, nullptr, permissions) ||
		    !add_acl_entry(acl, path, ACL_USER, &uid, permissions) ||
		    !add_acl_entry(acl, path, ACL_GROUP_OBJ, nullptr, 0) ||
		    !add_acl_entry(acl, path, ACL_MASK, nullptr, permissions) ||
		    !add_acl_entry(acl, path, ACL_OTHER, nullptr, 0)) {
			acl_free(acl);
			return false;
		}
		if (acl_valid(acl) != 0) {
			const int error_number = errno;
			acl_free(acl);
			return log_errno_failure("acl_valid for staged object", path, error_number);
		}
		if (set_fd_acl(fd, acl) != 0) {
			const int error_number = errno;
			acl_free(acl);
			return log_errno_failure("acl_set_fd for staged object", path, error_number);
		}
		acl_free(acl);
		const auto verification = verify_private_acl(fd, directory, uid);
		if (verification.status != AclVerifyStatus::kOk) {
			return log_acl_verify_failure(path, verification);
		}
		return true;
	}

#ifdef HOWDY_AUTH_HELPER_TESTING
	auto set_acl_setup_failure_for_test(bool fail) -> void {
		g_fail_acl_setup = fail;
	}

	auto set_acl_verification_failure_for_test(bool fail) -> void {
		g_fail_acl_verification = fail;
	}

	auto set_acl_io_for_test(AclSetFdForTest set_fd, AclGetFdForTest get_fd, AclResetForTest reset)
	    -> void {
		reset_acl_io_for_test();
		g_acl_set_fd_for_test = set_fd;
		g_acl_get_fd_for_test = get_fd;
		g_acl_reset_for_test  = reset;
	}

	auto reset_acl_io_for_test() -> void {
		if (g_acl_reset_for_test != nullptr) {
			g_acl_reset_for_test();
		}
		g_acl_set_fd_for_test = nullptr;
		g_acl_get_fd_for_test = nullptr;
		g_acl_reset_for_test  = nullptr;
	}
#endif

}  // namespace howdy::native::auth_helper
