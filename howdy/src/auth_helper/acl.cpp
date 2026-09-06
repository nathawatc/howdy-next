#include "auth_helper/acl.hpp"

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
		                               acl_perm_t permissions) -> AclVerifyResult {
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

		auto production_get_fd(void *context, int fd) -> acl_t {
			(void)context;
			return acl_get_fd(fd);
		}

		auto production_set_fd(void *context, int fd, acl_t acl) -> int {
			(void)context;
			return acl_set_fd(fd, acl);
		}

		constexpr auto acl_permissions(StagedAclPermissions permissions) -> acl_perm_t {
			return (permissions.read ? ACL_READ : 0) | (permissions.write ? ACL_WRITE : 0) |
			       (permissions.execute ? ACL_EXECUTE : 0);
		}

		auto inspect_acl(int fd, const StagedAclPolicy &policy, uid_t uid,
		                 const AclOperations &operations) -> AclVerifyResult {
			acl_t acl = operations.acl_get_fd(operations.context, fd);
			if (acl == nullptr) {
				return {.status       = AclVerifyStatus::kReadError,
				        .error_number = errno,
				        .operation    = "acl_get_fd"};
			}

			const std::array extended_tags = {ACL_USER_OBJ, ACL_USER, ACL_GROUP_OBJ, ACL_MASK,
			                                  ACL_OTHER};
			const std::array basic_tags    = {ACL_USER_OBJ, ACL_GROUP_OBJ, ACL_OTHER, acl_tag_t{},
			                                  acl_tag_t{}};
			const auto      &tags          = policy.has_named_target ? extended_tags : basic_tags;
			const std::array permissions   = {
			    policy.owner, policy.has_named_target ? policy.target : policy.group,
			    policy.has_named_target ? policy.group : policy.other, policy.mask, policy.other};
			const std::size_t expected_count = policy.has_named_target ? 5 : 3;
			std::size_t       entry_count    = 0;
			acl_entry_t       entry;
			int               entry_id = ACL_FIRST_ENTRY;
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
				if (entry_count >= expected_count) {
					acl_free(acl);
					return {.status = AclVerifyStatus::kMalformed};
				}
				const auto tag    = tags[entry_count];
				const auto result = acl_entry_has_permissions(
				    entry, tag, tag == ACL_USER ? static_cast<const void *>(&uid) : nullptr,
				    acl_permissions(permissions[entry_count]));
				if (result.status != AclVerifyStatus::kOk) {
					acl_free(acl);
					return result;
				}
				++entry_count;
			}
			acl_free(acl);
			return {.status = entry_count == expected_count ? AclVerifyStatus::kOk
			                                                : AclVerifyStatus::kMalformed};
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

	auto production_acl_operations() -> AclOperations {
		return {
		    .context = nullptr, .acl_get_fd = production_get_fd, .acl_set_fd = production_set_fd};
	}

	auto set_persistent_acl_with_operations(int fd, const std::filesystem::path &path, uid_t uid,
	                                        const StagedAclPolicy &policy,
	                                        const AclOperations   &operations) -> bool {
		if (operations.acl_get_fd == nullptr || operations.acl_set_fd == nullptr) {
			errno = EINVAL;
			return log_errno_failure("apply ACL to persistent object", path, errno);
		}
		acl_t acl = acl_init(policy.has_named_target ? 5 : 3);
		if (acl == nullptr) {
			return log_errno_failure("allocate ACL for persistent object", path, errno);
		}
		if (!add_acl_entry(acl, path, ACL_USER_OBJ, nullptr, acl_permissions(policy.owner)) ||
		    (policy.has_named_target &&
		     !add_acl_entry(acl, path, ACL_USER, &uid, acl_permissions(policy.target))) ||
		    !add_acl_entry(acl, path, ACL_GROUP_OBJ, nullptr, acl_permissions(policy.group)) ||
		    (policy.has_named_target &&
		     !add_acl_entry(acl, path, ACL_MASK, nullptr, acl_permissions(policy.mask))) ||
		    !add_acl_entry(acl, path, ACL_OTHER, nullptr, acl_permissions(policy.other)) ||
		    acl_valid(acl) != 0) {
			acl_free(acl);
			return false;
		}
		if (operations.acl_set_fd(operations.context, fd, acl) != 0) {
			const int error_number = errno;
			acl_free(acl);
			return log_errno_failure("acl_set_fd for persistent object", path, error_number);
		}
		acl_free(acl);
		return verify_persistent_acl_with_operations(fd, path, uid, policy, operations);
	}

	auto verify_persistent_acl_with_operations(int fd, const std::filesystem::path &path, uid_t uid,
	                                           const StagedAclPolicy &policy,
	                                           const AclOperations   &operations) -> bool {
		if (operations.acl_get_fd == nullptr) {
			errno = EINVAL;
			return log_errno_failure("verify ACL on persistent object", path, errno);
		}
		const auto result = inspect_acl(fd, policy, uid, operations);
		return result.status == AclVerifyStatus::kOk ? true : log_acl_verify_failure(path, result);
	}

}  // namespace howdy::native::auth_helper
