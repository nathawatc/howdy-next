#pragma once

#include "test_support.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

#include <sys/types.h>

#include <acl/libacl.h>

namespace howdy::test::auth_helper {

	using howdy::test::Expect;

	enum class AclCheckStatus : std::uint8_t {
		kMatch,
		kMismatch,
		kError,
	};

	struct AclCheckResult {
		AclCheckStatus   status;
		int              error_number = 0;
		std::string_view operation;
	};

	inline auto AclEntryHasPermissions(acl_entry_t entry, acl_tag_t tag, const void *qualifier,
	                                   int permissions) -> AclCheckResult {
		acl_tag_t entry_tag;
		if (acl_get_tag_type(entry, &entry_tag) != 0) {
			return {.status       = AclCheckStatus::kError,
			        .error_number = errno,
			        .operation    = "acl_get_tag_type"};
		}
		if (entry_tag != tag) {
			return {.status = AclCheckStatus::kMismatch};
		}
		if (qualifier != nullptr) {
			void *entry_qualifier = acl_get_qualifier(entry);
			if (entry_qualifier == nullptr) {
				return {.status       = AclCheckStatus::kError,
				        .error_number = errno,
				        .operation    = "acl_get_qualifier"};
			}
			const bool matches = std::memcmp(entry_qualifier, qualifier, sizeof(uid_t)) == 0;
			acl_free(entry_qualifier);
			if (!matches) {
				return {.status = AclCheckStatus::kMismatch};
			}
		}
		acl_permset_t permission_set;
		if (acl_get_permset(entry, &permission_set) != 0) {
			return {.status       = AclCheckStatus::kError,
			        .error_number = errno,
			        .operation    = "acl_get_permset"};
		}
		const int read_result = acl_get_perm(permission_set, ACL_READ);
		if (read_result < 0) {
			return {.status       = AclCheckStatus::kError,
			        .error_number = errno,
			        .operation    = "acl_get_perm"};
		}
		const int write_result = acl_get_perm(permission_set, ACL_WRITE);
		if (write_result < 0) {
			return {.status       = AclCheckStatus::kError,
			        .error_number = errno,
			        .operation    = "acl_get_perm"};
		}
		const int execute_result = acl_get_perm(permission_set, ACL_EXECUTE);
		if (execute_result < 0) {
			return {.status       = AclCheckStatus::kError,
			        .error_number = errno,
			        .operation    = "acl_get_perm"};
		}
		const bool matches = (read_result == 1) == ((permissions & ACL_READ) != 0) &&
		                     (write_result == 1) == ((permissions & ACL_WRITE) != 0) &&
		                     (execute_result == 1) == ((permissions & ACL_EXECUTE) != 0);
		return {.status = matches ? AclCheckStatus::kMatch : AclCheckStatus::kMismatch};
	}

	inline auto CheckPrivateAcl(const std::filesystem::path &path, uid_t uid, bool directory)
	    -> AclCheckResult {
		acl_t acl = acl_get_file(path.c_str(), ACL_TYPE_ACCESS);
		if (acl == nullptr) {
			return {.status       = AclCheckStatus::kError,
			        .error_number = errno,
			        .operation    = "acl_get_file"};
		}
		const int   permissions = directory ? (ACL_READ | ACL_EXECUTE) : ACL_READ;
		int         entry_count = 0;
		acl_entry_t entry;
		int         entry_id = ACL_FIRST_ENTRY;
		while (true) {
			const int entry_result = acl_get_entry(acl, entry_id, &entry);
			if (entry_result < 0) {
				const int error_number = errno;
				acl_free(acl);
				return {.status       = AclCheckStatus::kError,
				        .error_number = error_number,
				        .operation    = "acl_get_entry"};
			}
			if (entry_result == 0) {
				break;
			}
			entry_id = ACL_NEXT_ENTRY;
			++entry_count;
			acl_tag_t tag;
			if (acl_get_tag_type(entry, &tag) != 0) {
				const int error_number = errno;
				acl_free(acl);
				return {.status       = AclCheckStatus::kError,
				        .error_number = error_number,
				        .operation    = "acl_get_tag_type"};
			}
			const bool expected_tag = tag == ACL_USER_OBJ || tag == ACL_USER ||
			                          tag == ACL_GROUP_OBJ || tag == ACL_MASK || tag == ACL_OTHER;
			if (!expected_tag) {
				acl_free(acl);
				return {.status = AclCheckStatus::kMismatch};
			}
			const auto entry_check = AclEntryHasPermissions(
			    entry, tag, tag == ACL_USER ? static_cast<const void *>(&uid) : nullptr,
			    tag == ACL_GROUP_OBJ || tag == ACL_OTHER ? 0 : permissions);
			if (entry_check.status != AclCheckStatus::kMatch) {
				acl_free(acl);
				return entry_check;
			}
		}
		acl_free(acl);
		return {.status = entry_count == 5 ? AclCheckStatus::kMatch : AclCheckStatus::kMismatch};
	}

	inline auto ExpectPrivateAcl(const std::filesystem::path &path, uid_t uid, bool directory,
	                             const std::string &label) -> bool {
		const auto result = CheckPrivateAcl(path, uid, directory);
		if (result.status == AclCheckStatus::kError) {
			return Expect(false, label + " ACL check " + std::string(result.operation) + ": " +
			                         std::strerror(result.error_number));
		}
		return Expect(result.status == AclCheckStatus::kMatch, label + " ACL matches policy");
	}

}  // namespace howdy::test::auth_helper
