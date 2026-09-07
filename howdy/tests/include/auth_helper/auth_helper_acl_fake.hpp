#pragma once

#include "auth_helper/acl.hpp"
#include "test_support.hpp"

#include <cerrno>
#include <map>
#include <string>
#include <vector>

#include <sys/types.h>

#include <acl/libacl.h>

namespace howdy::test::auth_helper {

	using howdy::test::expect;

	struct FakeAclContext {
		std::map<int, acl_t> fake_acls;
		std::vector<int>     set_descriptors;
		std::vector<int>     get_descriptors;
		std::vector<uid_t>   target_uids;
		std::vector<int>     permissions;
		bool                 set_failure          = false;
		bool                 verification_failure = false;

		~FakeAclContext() {
			Clear();
		}

		void Clear() {
			for (const auto &[fd, acl] : fake_acls) {
				(void)fd;
				acl_free(acl);
			}
			fake_acls.clear();
			set_descriptors.clear();
			get_descriptors.clear();
			target_uids.clear();
			permissions.clear();
			set_failure          = false;
			verification_failure = false;
		}

		[[nodiscard]] auto Operations() -> howdy::native::auth_helper::AclOperations;
	};

	inline auto FakeAclSetFd(void *context, int fd, acl_t acl) -> int {
		auto &state = *static_cast<FakeAclContext *>(context);
		if (state.set_failure) {
			errno = EIO;
			return -1;
		}
		acl_t duplicate = acl_dup(acl);
		if (duplicate == nullptr) {
			return -1;
		}
		if (const auto existing = state.fake_acls.find(fd); existing != state.fake_acls.end()) {
			acl_free(existing->second);
			existing->second = duplicate;
		} else {
			state.fake_acls.emplace(fd, duplicate);
		}
		state.set_descriptors.push_back(fd);
		acl_entry_t entry;
		int         entry_id = ACL_FIRST_ENTRY;
		while (acl_get_entry(acl, entry_id, &entry) == 1) {
			entry_id = ACL_NEXT_ENTRY;
			acl_tag_t tag;
			if (acl_get_tag_type(entry, &tag) != 0 || tag != ACL_USER) {
				continue;
			}
			void *qualifier = acl_get_qualifier(entry);
			if (qualifier != nullptr) {
				state.target_uids.push_back(*static_cast<uid_t *>(qualifier));
				acl_free(qualifier);
			}
			acl_permset_t permission_set;
			if (acl_get_permset(entry, &permission_set) == 0) {
				state.permissions.push_back(
				    (acl_get_perm(permission_set, ACL_READ) == 1 ? ACL_READ : 0) |
				    (acl_get_perm(permission_set, ACL_WRITE) == 1 ? ACL_WRITE : 0) |
				    (acl_get_perm(permission_set, ACL_EXECUTE) == 1 ? ACL_EXECUTE : 0));
			}
		}
		return 0;
	}

	inline auto FakeAclGetFd(void *context, int fd) -> acl_t {
		auto &state = *static_cast<FakeAclContext *>(context);
		state.get_descriptors.push_back(fd);
		if (state.verification_failure) {
			return acl_init(0);
		}
		const auto stored = state.fake_acls.find(fd);
		if (stored == state.fake_acls.end()) {
			errno = ENODATA;
			return nullptr;
		}
		return acl_dup(stored->second);
	}

	inline auto FakeAclContext::Operations() -> howdy::native::auth_helper::AclOperations {
		return {.context = this, .acl_get_fd = FakeAclGetFd, .acl_set_fd = FakeAclSetFd};
	}

	inline auto ExpectFakeAclActivity(const FakeAclContext &state, const std::string &label)
	    -> bool {
		bool ok = true;
		ok &= expect(!state.fake_acls.empty(), label + " stores production ACLs by descriptor");
		ok &= expect(!state.set_descriptors.empty(), label + " receives production ACL");
		ok &= expect(state.get_descriptors.size() == state.set_descriptors.size(),
		             label + " verifies every stored ACL through fake readback");
		return ok;
	}

	inline auto FakeAclHasNamedUser(acl_t acl, uid_t uid) -> bool {
		acl_entry_t entry;
		int         entry_id = ACL_FIRST_ENTRY;
		while (acl_get_entry(acl, entry_id, &entry) == 1) {
			entry_id = ACL_NEXT_ENTRY;
			acl_tag_t tag;
			if (acl_get_tag_type(entry, &tag) != 0 || tag != ACL_USER) {
				continue;
			}
			void *qualifier = acl_get_qualifier(entry);
			if (qualifier == nullptr) {
				return false;
			}
			const bool matches = *static_cast<uid_t *>(qualifier) == uid;
			acl_free(qualifier);
			if (matches) {
				return true;
			}
		}
		return false;
	}

	inline auto ExpectFakeAclTarget(const FakeAclContext &state, uid_t target_uid, uid_t owner_uid)
	    -> bool {
		bool target_found = false;
		bool owner_found  = false;
		for (const auto &[fd, acl] : state.fake_acls) {
			(void)fd;
			target_found |= FakeAclHasNamedUser(acl, target_uid);
			owner_found |= FakeAclHasNamedUser(acl, owner_uid);
		}
		bool ok = expect(target_found, "production ACL contains requested named-user UID");
		if (target_uid != owner_uid) {
			ok &=
			    expect(!owner_found, "production ACL does not substitute owner UID for target UID");
		}
		return ok;
	}

	inline auto ExpectFakeAclDescriptorIsolation(FakeAclContext &state) -> bool {
		if (state.fake_acls.empty()) {
			return expect(false, "fake ACL descriptor isolation has stored ACL");
		}
		const int original_fd = state.fake_acls.begin()->first;
		errno                 = 0;
		acl_t wrong_acl       = FakeAclGetFd(&state, -1);
		bool  ok              = expect(wrong_acl == nullptr && errno == ENODATA,
		                               "fake ACL verification rejects different descriptor");
		if (wrong_acl != nullptr) {
			acl_free(wrong_acl);
		}
		acl_t original_acl = FakeAclGetFd(&state, original_fd);
		ok &= expect(original_acl != nullptr && acl_valid(original_acl) == 0,
		             "fake ACL verification accepts original descriptor");
		if (original_acl != nullptr) {
			acl_free(original_acl);
		}
		return ok;
	}

	inline auto ResetFakeAclBackend(FakeAclContext &state) -> bool {
		state.Clear();
		return expect(state.fake_acls.empty() && state.set_descriptors.empty() &&
		                  state.get_descriptors.empty(),
		              "fake ACL reset frees all descriptor ACLs and clears state");
	}

}  // namespace howdy::test::auth_helper
