#pragma once
#include "auth_helper/acl.hpp"
#include "auth_helper/runtime.hpp"
#include "test_support.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <grp.h>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <acl/libacl.h>

namespace howdy::test::auth_helper {

	using howdy::test::expect;
	constexpr acl_perm_t kAclRead    = acl_perm_t{ACL_READ};
	constexpr acl_perm_t kAclWrite   = acl_perm_t{ACL_WRITE};
	constexpr acl_perm_t kAclExecute = acl_perm_t{ACL_EXECUTE};

	class ScopedFd {
	public:
		ScopedFd() = default;

		explicit ScopedFd(int fd)
		    : fd_(fd) {}

		ScopedFd(const ScopedFd &)                     = delete;
		auto operator=(const ScopedFd &) -> ScopedFd & = delete;

		ScopedFd(ScopedFd &&other) noexcept
		    : fd_(other.release()) {}

		auto operator=(ScopedFd &&other) noexcept -> ScopedFd & {
			if (this != &other) {
				reset(other.release());
			}
			return *this;
		}

		~ScopedFd() {
			reset();
		}

		[[nodiscard]] auto get() const -> int {
			return fd_;
		}

		void reset(int fd = -1) {
			if (fd_ >= 0) {
				close(fd_);
			}
			fd_ = fd;
		}

		auto release() -> int {
			const int fd = fd_;
			fd_          = -1;
			return fd;
		}

	private:
		int fd_ = -1;
	};

	struct FakeAclContext {
		std::map<int, acl_t> fake_acls;
		std::vector<int>     set_descriptors;
		std::vector<int>     get_descriptors;
		std::vector<uid_t>   target_uids;
		std::vector<int>     permissions;
		bool                 set_failure          = false;
		bool                 verification_failure = false;

		~FakeAclContext() {
			clear();
		}

		void clear() {
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

		[[nodiscard]] auto operations() -> howdy::native::auth_helper::AclOperations;
	};

	inline auto fake_acl_set_fd(void *context, int fd, acl_t acl) -> int {
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

	inline auto fake_acl_get_fd(void *context, int fd) -> acl_t {
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

	inline auto FakeAclContext::operations() -> howdy::native::auth_helper::AclOperations {
		return {.context = this, .acl_get_fd = fake_acl_get_fd, .acl_set_fd = fake_acl_set_fd};
	}

	inline auto expect_fake_acl_activity(const FakeAclContext &state, const std::string &label)
	    -> bool {
		bool ok = true;
		ok &= expect(!state.fake_acls.empty(), label + " stores production ACLs by descriptor");
		ok &= expect(!state.set_descriptors.empty(), label + " receives production ACL");
		ok &= expect(state.get_descriptors.size() == state.set_descriptors.size(),
		             label + " verifies every stored ACL through fake readback");
		return ok;
	}

	inline auto fake_acl_has_named_user(acl_t acl, uid_t uid) -> bool {
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

	inline auto expect_fake_acl_target(const FakeAclContext &state, uid_t target_uid,
	                                   uid_t owner_uid) -> bool {
		bool target_found = false;
		bool owner_found  = false;
		for (const auto &[fd, acl] : state.fake_acls) {
			(void)fd;
			target_found |= fake_acl_has_named_user(acl, target_uid);
			owner_found |= fake_acl_has_named_user(acl, owner_uid);
		}
		bool ok = expect(target_found, "production ACL contains requested named-user UID");
		if (target_uid != owner_uid) {
			ok &=
			    expect(!owner_found, "production ACL does not substitute owner UID for target UID");
		}
		return ok;
	}

	inline auto expect_fake_acl_descriptor_isolation(FakeAclContext &state) -> bool {
		if (state.fake_acls.empty()) {
			return expect(false, "fake ACL descriptor isolation has stored ACL");
		}
		const int original_fd = state.fake_acls.begin()->first;
		errno                 = 0;
		acl_t wrong_acl       = fake_acl_get_fd(&state, -1);
		bool  ok              = expect(wrong_acl == nullptr && errno == ENODATA,
		                               "fake ACL verification rejects different descriptor");
		if (wrong_acl != nullptr) {
			acl_free(wrong_acl);
		}
		acl_t original_acl = fake_acl_get_fd(&state, original_fd);
		ok &= expect(original_acl != nullptr && acl_valid(original_acl) == 0,
		             "fake ACL verification accepts original descriptor");
		if (original_acl != nullptr) {
			acl_free(original_acl);
		}
		return ok;
	}

	inline auto reset_fake_acl_backend(FakeAclContext &state) -> bool {
		state.clear();
		return expect(state.fake_acls.empty() && state.set_descriptors.empty() &&
		                  state.get_descriptors.empty(),
		              "fake ACL reset frees all descriptor ACLs and clears state");
	}

	inline auto write_file(const std::filesystem::path &path, const std::string &content) -> bool {
		std::ofstream output(path);
		output << content;
		return output.good();
	}

	inline auto read_file(const std::filesystem::path &path) -> std::string {
		std::ifstream input(path);
		return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	}

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

	inline auto acl_entry_has_permissions(acl_entry_t entry, acl_tag_t tag, const void *qualifier,
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

	inline auto check_private_acl(const std::filesystem::path &path, uid_t uid, bool directory)
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
			const auto entry_check = acl_entry_has_permissions(
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

	inline auto expect_private_acl(const std::filesystem::path &path, uid_t uid, bool directory,
	                               const std::string &label) -> bool {
		const auto result = check_private_acl(path, uid, directory);
		if (result.status == AclCheckStatus::kError) {
			return expect(false, label + " ACL check " + std::string(result.operation) + ": " +
			                         std::strerror(result.error_number));
		}
		return expect(result.status == AclCheckStatus::kMatch, label + " ACL matches policy");
	}

	inline auto
	child_can_access_staged_files(const howdy::native::auth_helper::PreparedPaths &prepared,
	                              uid_t uid, gid_t gid, bool expect_access) -> bool {
		const pid_t child = fork();
		if (child == 0) {
			if (setgroups(0, nullptr) != 0 || setgid(gid) != 0 || setuid(uid) != 0) {
				_exit(2);
			}
			const int runtime_fd =
			    open(prepared.runtime_dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
			const int models_fd =
			    open(prepared.user_models_dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
			const int config_fd = open(prepared.config_path.c_str(), O_RDONLY | O_CLOEXEC);
			const int model_fd =
			    open((prepared.user_models_dir / "alice.dat").c_str(), O_RDONLY | O_CLOEXEC);
			if (runtime_fd >= 0) {
				close(runtime_fd);
			}
			if (models_fd >= 0) {
				close(models_fd);
			}
			if (config_fd >= 0) {
				close(config_fd);
			}
			if (model_fd >= 0) {
				close(model_fd);
			}
			const bool accessible =
			    runtime_fd >= 0 && models_fd >= 0 && config_fd >= 0 && model_fd >= 0;
			_exit(accessible == expect_access ? 0 : 1);
		}
		if (child < 0) {
			return false;
		}
		int status = 0;
		return waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
	}

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
