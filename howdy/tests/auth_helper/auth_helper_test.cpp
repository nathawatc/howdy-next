#include "auth_helper/runtime.hpp"
#include "protocol/auth_helper_protocol.hpp"
#include "support/auth_helper_testing.hpp"
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
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <acl/libacl.h>

namespace {

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

	std::map<int, acl_t> fake_acls;
	int                  fake_acl_set_calls = 0;
	int                  fake_acl_get_calls = 0;

	auto fake_acl_set_fd(int fd, acl_t acl) -> int {
		acl_t duplicate = acl_dup(acl);
		if (duplicate == nullptr) {
			return -1;
		}
		if (const auto existing = fake_acls.find(fd); existing != fake_acls.end()) {
			acl_free(existing->second);
			existing->second = duplicate;
		} else {
			fake_acls.emplace(fd, duplicate);
		}
		++fake_acl_set_calls;
		return 0;
	}

	auto fake_acl_get_fd(int fd) -> acl_t {
		++fake_acl_get_calls;
		const auto stored = fake_acls.find(fd);
		if (stored == fake_acls.end()) {
			errno = ENODATA;
			return nullptr;
		}
		return acl_dup(stored->second);
	}

	auto reset_fake_acl_state() -> void {
		for (const auto &[fd, acl] : fake_acls) {
			(void)fd;
			acl_free(acl);
		}
		fake_acls.clear();
		fake_acl_set_calls = 0;
		fake_acl_get_calls = 0;
	}

	auto install_fake_acl_backend() -> void {
		howdy::native::auth_helper::set_acl_io_for_test(fake_acl_set_fd, fake_acl_get_fd,
		                                                reset_fake_acl_state);
	}

	auto expect_fake_acl_activity(const std::string &label) -> bool {
		bool ok = true;
		ok &= expect(!fake_acls.empty(), label + " stores production ACLs by descriptor");
		ok &= expect(fake_acl_set_calls > 0, label + " receives production ACL");
		ok &= expect(fake_acl_get_calls == fake_acl_set_calls,
		             label + " verifies every stored ACL through fake readback");
		return ok;
	}

	auto fake_acl_has_named_user(acl_t acl, uid_t uid) -> bool {
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

	auto expect_fake_acl_target(uid_t target_uid, uid_t owner_uid) -> bool {
		bool target_found = false;
		bool owner_found  = false;
		for (const auto &[fd, acl] : fake_acls) {
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

	auto expect_fake_acl_descriptor_isolation() -> bool {
		if (fake_acls.empty()) {
			return expect(false, "fake ACL descriptor isolation has stored ACL");
		}
		const int original_fd = fake_acls.begin()->first;
		errno                 = 0;
		acl_t wrong_acl       = fake_acl_get_fd(-1);
		bool  ok              = expect(wrong_acl == nullptr && errno == ENODATA,
		                               "fake ACL verification rejects different descriptor");
		if (wrong_acl != nullptr) {
			acl_free(wrong_acl);
		}
		acl_t original_acl = fake_acl_get_fd(original_fd);
		ok &= expect(original_acl != nullptr && acl_valid(original_acl) == 0,
		             "fake ACL verification accepts original descriptor");
		if (original_acl != nullptr) {
			acl_free(original_acl);
		}
		return ok;
	}

	auto reset_fake_acl_backend() -> bool {
		howdy::native::auth_helper::reset_acl_io_for_test();
		return expect(fake_acls.empty() && fake_acl_set_calls == 0 && fake_acl_get_calls == 0,
		              "fake ACL reset frees all descriptor ACLs and clears state");
	}

	auto write_file(const std::filesystem::path &path, const std::string &content) -> bool {
		std::ofstream output(path);
		output << content;
		return output.good();
	}

	auto read_file(const std::filesystem::path &path) -> std::string {
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

	auto acl_entry_has_permissions(acl_entry_t entry, acl_tag_t tag, const void *qualifier,
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

	auto check_private_acl(const std::filesystem::path &path, uid_t uid, bool directory)
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

	auto expect_private_acl(const std::filesystem::path &path, uid_t uid, bool directory,
	                        const std::string &label) -> bool {
		const auto result = check_private_acl(path, uid, directory);
		if (result.status == AclCheckStatus::kError) {
			return expect(false, label + " ACL check " + std::string(result.operation) + ": " +
			                         std::strerror(result.error_number));
		}
		return expect(result.status == AclCheckStatus::kMatch, label + " ACL matches policy");
	}

	auto child_can_access_staged_files(const howdy::native::auth_helper::PreparedPaths &prepared,
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

	auto add_acl_probe_entry(acl_t &acl, acl_tag_t tag, const void *qualifier,
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

	auto configure_acl_probe(acl_t &acl, int fd, const std::filesystem::path &probe_path,
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

	auto acl_support(const std::filesystem::path &root,
	                 std::optional<int>           injected_error = std::nullopt) -> AclSupport {
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

	auto expect_acl_probe_classification(const std::filesystem::path &temp_root) -> bool {
		std::ostringstream output;
		auto              *previous_cerr = std::cerr.rdbuf(output.rdbuf());
		const auto         eopnotsupp    = acl_support(temp_root, EOPNOTSUPP);
		const auto         enotsupp      = acl_support(temp_root, ENOTSUP);
		const auto         eio           = acl_support(temp_root, EIO);
		std::cerr.rdbuf(previous_cerr);
		bool ok = true;
		ok &= expect(eopnotsupp == AclSupport::kUnsupported,
		             "EOPNOTSUPP ACL probe result skips ACL tests");
		ok &= expect(enotsupp == AclSupport::kUnsupported,
		             "ENOTSUP ACL probe result skips ACL tests");
		ok &= expect(eio == AclSupport::kError, "unexpected ACL probe error fails suite");
		return ok;
	}

	auto runtime_dirs_for_uid(const std::filesystem::path &runtime_root, uid_t uid)
	    -> std::set<std::filesystem::path> {
		namespace fs = std::filesystem;

		std::set<fs::path> paths;
		std::error_code    ec;
		const auto         prefix = "pam-" + std::to_string(uid) + "-";
		for (const auto &entry : fs::directory_iterator(runtime_root, ec)) {
			if (entry.path().filename().string().starts_with(prefix)) {
				paths.insert(entry.path());
			}
		}
		return paths;
	}

	auto expect_runtime_root_validation(const std::filesystem::path &temp_root) -> bool {
		using howdy::native::testing::runtime_root;
		using howdy::native::testing::validate_runtime_root;

		bool ok = true;
		ok &= expect(runtime_root() == "/run/howdy", "runtime root is fixed under /run/howdy");

		const auto regular_path = temp_root / "runtime-root-file";
		ok &= expect(write_file(regular_path, "not a directory"), "writes runtime root file");
		ok &= expect(!validate_runtime_root(regular_path), "regular runtime root path is rejected");

		const auto      user_owned_dir = temp_root / "runtime-root-dir";
		std::error_code ec;
		std::filesystem::create_directory(user_owned_dir, ec);
		ok &= expect(!ec, "creates runtime root directory fixture");
		if (geteuid() == 0) {
			ok &= expect(validate_runtime_root(user_owned_dir),
			             "root-owned runtime root directory is accepted");
		} else {
			ok &= expect(!validate_runtime_root(user_owned_dir),
			             "user-owned runtime root directory is rejected");
		}

		return ok;
	}

	auto expect_secure_source_file_stat(const std::filesystem::path &temp_root) -> bool {
		using howdy::native::testing::secure_source_file_stat;

		bool ok = true;
		ok &= expect(!secure_source_file_stat(-1, "Invalid fd"), "invalid fd is rejected");

		ScopedFd dir_fd(open(temp_root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
		ok &= expect(dir_fd.get() >= 0, "opens directory fd");
		ok &= expect(!secure_source_file_stat(dir_fd.get(), "Directory"),
		             "directory fd is rejected as source file");

		const auto regular_path = temp_root / "source-file";
		ok &= expect(write_file(regular_path, "source"), "writes source file");
		ok &= expect(chmod(regular_path.c_str(), 0664) == 0, "makes source group-writable");
		ScopedFd group_writable_fd(open(regular_path.c_str(), O_RDONLY | O_CLOEXEC));
		ok &= expect(group_writable_fd.get() >= 0, "opens group-writable source");
		ok &= expect(!secure_source_file_stat(group_writable_fd.get(), "Group writable source"),
		             "group-writable source is rejected");
		group_writable_fd.reset();

		ok &= expect(chmod(regular_path.c_str(), 0644) == 0, "restores source mode");
		ScopedFd regular_fd(open(regular_path.c_str(), O_RDONLY | O_CLOEXEC));
		ok &= expect(regular_fd.get() >= 0, "opens regular source");
		if (geteuid() == 0) {
			ok &= expect(secure_source_file_stat(regular_fd.get(), "Root source"),
			             "root-owned regular source is accepted");
		} else {
			ok &= expect(!secure_source_file_stat(regular_fd.get(), "User source"),
			             "non-root-owned regular source is rejected");
		}

		return ok;
	}

	auto expect_write_all_helper(const std::filesystem::path &temp_root) -> bool {
		using howdy::native::testing::write_all;

		bool ok = true;
		ok &= expect(!write_all(-1, "x", 1), "invalid write fd is rejected");

		const auto output_path = temp_root / "write-all-output";
		ScopedFd   output_fd(
		    open(output_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
		ok &= expect(output_fd.get() >= 0, "creates write-all output file");
		constexpr auto kOutput = "alpha\nbeta\n";
		ok &=
		    expect(write_all(output_fd.get(), kOutput, static_cast<ssize_t>(std::strlen(kOutput))),
		           "writes complete buffer");
		output_fd.reset();
		ok &= expect(read_file(output_path) == kOutput, "write-all output matches input");

		return ok;
	}

	auto expect_copy_file_for_user(const std::filesystem::path &temp_root, bool acl_functional,
	                               bool real_acl_supported) -> bool {
		using howdy::native::testing::copy_file_for_user;

		bool ok = true;
		ok &= expect(
		    !copy_file_for_user(temp_root / "missing", temp_root / "dest", "Missing", getgid()),
		    "missing source copy fails");

		const auto real_source    = temp_root / "real-source";
		const auto symlink_source = temp_root / "symlink-source";
		ok &= expect(write_file(real_source, "real"), "writes real source");
		if (symlink(real_source.c_str(), symlink_source.c_str()) == 0) {
			ok &= expect(!copy_file_for_user(symlink_source, temp_root / "symlink-dest", "Symlink",
			                                 getgid()),
			             "symlink source copy fails closed");
		} else {
			std::cerr << "SKIP: symlink source creation failed: " << std::strerror(errno) << "\n";
		}

		if (geteuid() == 0 && acl_functional) {
			const auto destination = temp_root / "copied-source";
			ok &= expect(chmod(real_source.c_str(), 0644) == 0, "sets secure source mode");
			ok &= expect(copy_file_for_user(real_source, destination, "Source", getgid()),
			             "secure source copy succeeds as root");
			ok &= expect(read_file(destination) == "real", "copied file preserves content");

			struct stat stat_{};
			ok &= expect(lstat(destination.c_str(), &stat_) == 0, "stats copied file");
			ok &= expect(stat_.st_uid == 0 && stat_.st_gid == 0, "copied file owner is root:root");
			if (real_acl_supported) {
				ok &= expect_private_acl(destination, geteuid(), false, "copied file");
			}
			ok &= expect(!copy_file_for_user(real_source, destination, "Existing", getgid()),
			             "existing destination copy fails");
		} else if (geteuid() != 0) {
			std::cerr << "SKIP: successful auth-helper copy requires root-owned source\n";
		}

		return ok;
	}

	auto expect_fake_acl_backend(const std::filesystem::path &temp_root) -> bool {
		using howdy::native::auth_helper::set_acl_setup_failure_for_test;
		using howdy::native::auth_helper::set_acl_verification_failure_for_test;
		using howdy::native::testing::copy_file_for_user;

		if (geteuid() != 0) {
			std::cerr << "SKIP: fake ACL production-copy test requires root-owned source\n";
			return true;
		}

		bool            ok = true;
		std::error_code ec;
		const auto      source = temp_root / "fake-acl-source";
		ok &= expect(write_file(source, "source"), "writes fake ACL source");
		ok &= expect(chmod(source.c_str(), 0644) == 0, "secures fake ACL source");

		install_fake_acl_backend();
		const auto success_destination = temp_root / "fake-acl-success";
		ok &= expect(copy_file_for_user(source, success_destination, "Fake ACL source", getgid()),
		             "production ACL setup succeeds through fake backend");
		ok &= expect_fake_acl_activity("fake ACL backend");
		ok &= expect_fake_acl_descriptor_isolation();
		ok &= reset_fake_acl_backend();

		install_fake_acl_backend();
		set_acl_setup_failure_for_test(true);
		const auto setup_failure_destination = temp_root / "fake-acl-setup-failure";
		ok &= expect(
		    !copy_file_for_user(source, setup_failure_destination, "Fake ACL source", getgid()),
		    "ACL setup injection propagates through fake backend");
		set_acl_setup_failure_for_test(false);
		ok &= expect(fake_acl_set_calls == 0 && fake_acl_get_calls == 0,
		             "setup injection fails before fake ACL I/O");
		ok &= reset_fake_acl_backend();

		install_fake_acl_backend();
		set_acl_verification_failure_for_test(true);
		const auto verification_failure_destination = temp_root / "fake-acl-verification-failure";
		ok &= expect(!copy_file_for_user(source, verification_failure_destination,
		                                 "Fake ACL source", getgid()),
		             "ACL verification injection propagates through fake backend");
		set_acl_verification_failure_for_test(false);
		ok &= expect(fake_acl_set_calls == 1 && fake_acl_get_calls == 0,
		             "verification injection follows production ACL setup");
		ok &= reset_fake_acl_backend();

		std::filesystem::remove(success_destination, ec);
		ec.clear();
		std::filesystem::remove(setup_failure_destination, ec);
		ec.clear();
		std::filesystem::remove(verification_failure_destination, ec);
		return ok;
	}

	auto expect_source_model_readiness(const std::filesystem::path &temp_root, bool acl_functional)
	    -> bool {
		namespace fs = std::filesystem;
		using howdy::native::testing::copy_file_for_user;
		using howdy::native::testing::select_source_model_path;

		bool                    ok = true;
		std::error_code         ec;
		std::optional<fs::path> selected_path;
		const auto              missing_dir = temp_root / "missing-source-models";

		selected_path = temp_root / "unexpected.dat";
		ok &= expect(select_source_model_path(missing_dir, "alice", std::nullopt, selected_path),
		             "missing source models directory is treated as no staged model");
		ok &=
		    expect(!selected_path.has_value(), "missing source models directory selects no model");

		const auto models_dir = temp_root / "source-models";
		const auto model_path = models_dir / "alice.dat";
		fs::create_directory(models_dir, ec);
		ok &= expect(!ec, "creates source models directory");
		ok &= expect(chmod(models_dir.c_str(), 0755) == 0, "secures source models directory");

		selected_path = temp_root / "unexpected.dat";
		ok &= expect(!select_source_model_path(models_dir, "../alice", std::nullopt, selected_path),
		             "invalid source model user fails closed");
		ok &= expect(!selected_path.has_value(),
		             "invalid source model user clears stale selected path");

		selected_path = temp_root / "unexpected.dat";
		ok &= expect(select_source_model_path(models_dir, "alice", std::nullopt, selected_path),
		             "missing source model file preserves prepare behavior");
		ok &= expect(!selected_path.has_value(), "missing source model file selects no model");

		ok &= expect(write_file(model_path, "not-json"), "writes secure source model");
		ok &= expect(chmod(model_path.c_str(), 0644) == 0, "secures source model file");
		ok &= expect(select_source_model_path(models_dir, "alice", std::nullopt, selected_path),
		             "secure source model is accepted without parsing");
		ok &= expect(selected_path == model_path, "secure source model path is selected");

		const auto relative_models_dir = fs::relative(models_dir, fs::current_path(), ec);
		ok &= expect(!ec, "resolves relative source models directory");
		selected_path = temp_root / "unexpected.dat";
		ok &=
		    expect(select_source_model_path(relative_models_dir, "alice", geteuid(), selected_path),
		           "secure source model is accepted with calling uid owner check");
		ok &=
		    expect(selected_path.has_value(), "calling uid owner check selects source model path");
		ok &= expect(selected_path.has_value() && selected_path->filename() == "alice.dat",
		             "calling uid owner check selects source model filename");
		ok &= expect(selected_path.has_value() && fs::equivalent(*selected_path, model_path, ec),
		             "calling uid owner check selects equivalent source model path");
		ec.clear();

		struct stat model_stat{};
		ok &= expect(lstat(model_path.c_str(), &model_stat) == 0, "stats secure source model");
		const uid_t mismatched_owner =
		    model_stat.st_uid == 0 ? static_cast<uid_t>(1) : static_cast<uid_t>(0);
		selected_path = temp_root / "unexpected.dat";
		ok &= expect(!select_source_model_path(relative_models_dir, "alice", mismatched_owner,
		                                       selected_path),
		             "secure source model with mismatched owner uid fails closed");
		ok &= expect(!selected_path.has_value(), "mismatched owner uid clears stale selected path");

		if (geteuid() == 0) {
			selected_path = temp_root / "unexpected.dat";
			ok &= expect(select_source_model_path(relative_models_dir, "alice",
			                                      static_cast<uid_t>(0), selected_path),
			             "secure source model is accepted with root owner check");
			ok &= expect(selected_path.has_value(), "root owner check selects source model path");
			ok &= expect(selected_path.has_value() && selected_path->filename() == "alice.dat",
			             "root owner check selects source model filename");
			ok &=
			    expect(selected_path.has_value() && fs::equivalent(*selected_path, model_path, ec),
			           "root owner check selects equivalent source model path");
			ec.clear();
		}

		if (geteuid() == 0 && acl_functional) {
			const auto staged_path = temp_root / "staged-alice.dat";
			ok &= expect(copy_file_for_user(model_path, staged_path, "User model file", getgid()),
			             "secure source model is staged");
			ok &= expect(fs::exists(staged_path), "staged source model exists");
		} else if (geteuid() != 0) {
			std::cerr << "SKIP: successful auth-helper staging requires root-owned source\n";
		}

		const auto symlink_target = models_dir / "target.dat";
		ok &= expect(write_file(symlink_target, "target"), "writes source model symlink target");
		fs::remove(model_path, ec);
		ec.clear();
		if (symlink(symlink_target.c_str(), model_path.c_str()) == 0) {
			selected_path = temp_root / "unexpected.dat";
			ok &=
			    expect(!select_source_model_path(models_dir, "alice", std::nullopt, selected_path),
			           "symlinked source model file fails closed");
			ok &= expect(!selected_path.has_value(),
			             "symlinked source model file clears stale selected path");
			fs::remove(model_path, ec);
			ec.clear();
		} else {
			std::cerr << "SKIP: source model symlink creation failed: " << std::strerror(errno)
			          << "\n";
		}

		const auto real_models_dir    = temp_root / "real-source-models";
		const auto symlink_models_dir = temp_root / "symlink-source-models";
		fs::create_directory(real_models_dir, ec);
		ok &= expect(!ec, "creates real source models directory");
		ok &= expect(chmod(real_models_dir.c_str(), 0755) == 0,
		             "secures real source models directory");
		if (symlink(real_models_dir.c_str(), symlink_models_dir.c_str()) == 0) {
			selected_path = temp_root / "unexpected.dat";
			ok &= expect(
			    !select_source_model_path(symlink_models_dir, "alice", std::nullopt, selected_path),
			    "symlinked source models directory fails closed");
			ok &= expect(!selected_path.has_value(),
			             "symlinked source models directory clears stale selected path");
			fs::remove(symlink_models_dir, ec);
			ec.clear();
		} else {
			std::cerr << "SKIP: source models directory symlink creation failed: "
			          << std::strerror(errno) << "\n";
		}

		ok &= expect(write_file(model_path, "not-json"), "restores secure source model");
		ok &=
		    expect(chmod(model_path.c_str(), 0664) == 0, "makes source model file group-writable");
		selected_path = temp_root / "unexpected.dat";
		ok &= expect(!select_source_model_path(models_dir, "alice", std::nullopt, selected_path),
		             "group-writable source model file fails closed");
		ok &= expect(!selected_path.has_value(),
		             "group-writable source model file clears stale selected path");
		ok &=
		    expect(chmod(model_path.c_str(), 0666) == 0, "makes source model file world-writable");
		ok &= expect(!select_source_model_path(models_dir, "alice", std::nullopt, selected_path),
		             "world-writable source model file fails closed");
		ok &= expect(chmod(model_path.c_str(), 0644) == 0, "restores source model file mode");

		ok &= expect(chmod(models_dir.c_str(), 0775) == 0,
		             "makes source models directory group-writable");
		selected_path = temp_root / "unexpected.dat";
		ok &= expect(!select_source_model_path(models_dir, "alice", std::nullopt, selected_path),
		             "group-writable source models directory fails closed");
		ok &= expect(!selected_path.has_value(),
		             "group-writable source models directory clears stale selected path");
		ok &= expect(chmod(models_dir.c_str(), 0777) == 0,
		             "makes source models directory world-writable");
		ok &= expect(!select_source_model_path(models_dir, "alice", std::nullopt, selected_path),
		             "world-writable source models directory fails closed");
		ok &= expect(chmod(models_dir.c_str(), 0755) == 0, "restores source models directory mode");

		return ok;
	}

	auto expect_prepare_cleanup_guards() -> bool {
		using howdy::native::testing::cleanup_for_user;
		using howdy::native::testing::prepare_for_user;

		bool ok = true;
		if (geteuid() == 0) {
			ok &= expect(prepare_for_user("../alice") == 1, "root prepare rejects invalid user");
			ok &= expect(cleanup_for_user("/tmp/not-howdy-runtime") == 1,
			             "root cleanup rejects unexpected path");
			const auto missing_expected = std::filesystem::path("/run/howdy") /
			                              ("pam-" + std::to_string(getuid()) + "-missing");
			ok &= expect(cleanup_for_user(missing_expected) == 0,
			             "root cleanup accepts missing expected runtime dir");
		} else {
			ok &= expect(prepare_for_user("../alice") == 1, "non-root prepare fails closed");
			ok &= expect(cleanup_for_user("/run/howdy/pam-0-missing") == 1,
			             "non-root cleanup fails closed");
		}

		return ok;
	}

	auto expect_cleanup_runtime_auth_files(const std::filesystem::path &temp_root) -> bool {
		namespace fs = std::filesystem;
		using howdy::native::auth_helper::cleanup_runtime_auth_files_for_test;

		bool            ok = true;
		std::error_code ec;
		const auto      fixture_root = temp_root / "cleanup-runtime-auth-files";
		const auto      runtime_root = fixture_root / "runtime-root";
		const auto      uid          = getuid();
		const auto      gid          = getgid();
		const howdy::native::auth_helper::RuntimeIdentity identity{.uid = uid, .gid = gid};
		const auto  prefix       = "pam-" + std::to_string(uid) + "-";
		const uid_t non_root_uid = 1;
		const gid_t wrong_gid    = gid == 0 ? static_cast<gid_t>(1) : static_cast<gid_t>(0);

		fs::remove_all(fixture_root, ec);
		fs::create_directories(runtime_root, ec);
		ok &= expect(!ec, "creates cleanup runtime fixtures");
		ok &= expect(chmod(runtime_root.c_str(), 0711) == 0, "secures cleanup runtime root");

		const auto wrong_parent = fixture_root / "wrong-parent" / (prefix + "wrong-parent");
		ok &= expect(!cleanup_runtime_auth_files_for_test(wrong_parent, identity, runtime_root).ok,
		             "wrong cleanup parent is rejected");

		const auto wrong_prefix = runtime_root / ("pam-" + std::to_string(uid + 1) + "-wrong");
		ok &= expect(!cleanup_runtime_auth_files_for_test(wrong_prefix, identity, runtime_root).ok,
		             "wrong pam uid prefix is rejected");

		const auto missing_runtime_dir = runtime_root / (prefix + "missing");
		ok &= expect(
		    cleanup_runtime_auth_files_for_test(missing_runtime_dir, identity, runtime_root).ok,
		    "missing expected runtime dir succeeds");
		ok &=
		    expect(!fs::exists(missing_runtime_dir), "missing expected runtime dir stays missing");

		const auto regular_file = runtime_root / (prefix + "regular");
		ok &= expect(write_file(regular_file, "cleanup"), "writes runtime cleanup file");
		ok &= expect(!cleanup_runtime_auth_files_for_test(regular_file, identity, runtime_root).ok,
		             "regular file is rejected for cleanup");
		ok &= expect(fs::exists(regular_file), "regular file remains after rejected cleanup");
		fs::remove(regular_file, ec);
		ec.clear();

		const auto symlink_target = fixture_root / "cleanup-target";
		const auto symlink_path   = runtime_root / (prefix + "symlink");
		ok &= expect(write_file(symlink_target, "target"), "writes cleanup symlink target");
		if (symlink(symlink_target.c_str(), symlink_path.c_str()) == 0) {
			ok &= expect(
			    !cleanup_runtime_auth_files_for_test(symlink_path, identity, runtime_root).ok,
			    "symlink is rejected for cleanup");
			ok &= expect(fs::exists(symlink_path), "symlink remains after rejected cleanup");
			fs::remove(symlink_path, ec);
			ec.clear();
		} else {
			std::cerr << "SKIP: cleanup symlink fixture creation failed: " << std::strerror(errno)
			          << "\n";
		}
		fs::remove(symlink_target, ec);
		ec.clear();

		const auto privileged_probe = fixture_root / "privileged-cleanup-probe";
		fs::create_directories(privileged_probe, ec);
		ok &= expect(!ec, "creates privileged cleanup probe");
		const bool can_setup_privileged_cleanup =
		    chown(privileged_probe.c_str(), non_root_uid, gid) == 0 &&
		    chown(privileged_probe.c_str(), 0, wrong_gid) == 0 &&
		    chown(privileged_probe.c_str(), 0, gid) == 0;
		const bool      restored_privileged_probe = chown(privileged_probe.c_str(), uid, gid) == 0;
		std::error_code privileged_probe_cleanup_ec;
		fs::remove_all(privileged_probe, privileged_probe_cleanup_ec);
		const bool cleaned_up_privileged_probe =
		    restored_privileged_probe && !privileged_probe_cleanup_ec;
		ec.clear();

		if (!can_setup_privileged_cleanup) {
			std::cerr << "SKIP: cleanup ownership/mode cases need required chown capabilities\n";
		} else {
			const bool privileged_probe_cleanup_ok =
			    expect(cleaned_up_privileged_probe, "cleans up privileged cleanup probe");
			ok &= privileged_probe_cleanup_ok;
			if (privileged_probe_cleanup_ok) {
				const auto group_writable_dir = runtime_root / (prefix + "group-writable");
				fs::create_directories(group_writable_dir, ec);
				ok &= expect(!ec, "creates group-writable cleanup directory");
				ok &= expect(chown(group_writable_dir.c_str(), 0, gid) == 0,
				             "sets root-owned group-writable cleanup directory");
				ok &= expect(chmod(group_writable_dir.c_str(), 0770) == 0,
				             "makes cleanup directory group-writable");
				ok &= expect(
				    !cleanup_runtime_auth_files_for_test(group_writable_dir, identity, runtime_root)
				         .ok,
				    "group-writable directory is rejected");
				fs::remove_all(group_writable_dir, ec);
				ec.clear();

				const auto world_writable_dir = runtime_root / (prefix + "world-writable");
				fs::create_directories(world_writable_dir, ec);
				ok &= expect(!ec, "creates world-writable cleanup directory");
				ok &= expect(chown(world_writable_dir.c_str(), 0, gid) == 0,
				             "sets root-owned world-writable cleanup directory");
				ok &= expect(chmod(world_writable_dir.c_str(), 0777) == 0,
				             "makes cleanup directory world-writable");
				ok &= expect(
				    !cleanup_runtime_auth_files_for_test(world_writable_dir, identity, runtime_root)
				         .ok,
				    "world-writable directory is rejected");
				fs::remove_all(world_writable_dir, ec);
				ec.clear();

				const auto valid_runtime_dir = runtime_root / (prefix + "valid");
				fs::create_directories(valid_runtime_dir, ec);
				ok &= expect(!ec, "creates valid cleanup directory");
				ok &= expect(chown(valid_runtime_dir.c_str(), 0, gid) == 0,
				             "sets root-owned valid cleanup directory");
				ok &= expect(chmod(valid_runtime_dir.c_str(), 0711) == 0,
				             "secures valid cleanup directory");
				ok &= expect(
				    cleanup_runtime_auth_files_for_test(valid_runtime_dir, identity, runtime_root)
				        .ok,
				    "valid runtime directory is removed");
				ok &= expect(!fs::exists(valid_runtime_dir), "valid runtime directory is gone");

				const auto wrong_owner_dir = runtime_root / (prefix + "wrong-owner");
				fs::create_directories(wrong_owner_dir, ec);
				ok &= expect(!ec, "creates wrong-owner cleanup directory");
				ok &= expect(chown(wrong_owner_dir.c_str(), non_root_uid, gid) == 0,
				             "sets wrong-owner cleanup directory");
				ok &= expect(
				    !cleanup_runtime_auth_files_for_test(wrong_owner_dir, identity, runtime_root)
				         .ok,
				    "wrong owner is rejected");
				fs::remove_all(wrong_owner_dir, ec);
				ec.clear();

				const auto wrong_gid_dir = runtime_root / (prefix + "wrong-gid");
				fs::create_directories(wrong_gid_dir, ec);
				ok &= expect(!ec, "creates wrong-gid cleanup directory");
				ok &= expect(chown(wrong_gid_dir.c_str(), 0, wrong_gid) == 0,
				             "sets wrong-gid cleanup directory");
				ok &= expect(
				    !cleanup_runtime_auth_files_for_test(wrong_gid_dir, identity, runtime_root).ok,
				    "wrong gid is rejected");
				fs::remove_all(wrong_gid_dir, ec);
				ec.clear();
			}
		}

		fs::remove_all(fixture_root, ec);
		ok &= expect(!ec, "cleans cleanup runtime fixtures");
		return ok;
	}

	auto expect_prepare_runtime_auth_files(const std::filesystem::path &temp_root,
	                                       bool acl_functional, uid_t target_uid) -> bool {
		namespace fs = std::filesystem;
		using howdy::native::auth_helper::prepare_runtime_auth_files_for_test;

		bool            ok = true;
		std::error_code ec;
		const auto      fixture_dir  = temp_root / "prepare-runtime-auth-files";
		const auto      runtime_root = fixture_dir / "runtime-root";
		const auto      source_dir   = fixture_dir / "source";
		const auto      original_dir = fs::current_path();
		const auto      config_path  = fs::path("./config.ini");
		const auto      models_dir   = fs::path("./models");
		const auto      model_path   = models_dir / "alice.dat";
		const uid_t     owner_uid    = geteuid();
		const howdy::native::auth_helper::RuntimeIdentity      identity{.uid = target_uid,
		                                                                .gid = getgid()};
		const howdy::native::auth_helper::RuntimeAuthTestPaths source_paths{
		    .runtime_root           = runtime_root,
		    .source_config          = config_path,
		    .source_user_models_dir = models_dir,
		};
		fs::remove_all(fixture_dir, ec);
		fs::create_directories(runtime_root, ec);
		ok &= expect(!ec, "creates injected runtime root");
		ok &= expect(chmod(runtime_root.c_str(), 0711) == 0, "secures injected runtime root");
		fs::create_directories(source_dir / "models", ec);
		ok &= expect(!ec, "creates prepare runtime auth files fixtures");
		fs::current_path(source_dir, ec);
		ok &= expect(!ec, "uses relative secure source paths");
		ok &= expect(write_file(config_path, "config-content"), "writes secure source config");
		ok &= expect(chmod(fixture_dir.c_str(), 0755) == 0, "secures source fixture directory");
		ok &= expect(chmod(source_dir.c_str(), 0755) == 0, "secures source directory");
		ok &= expect(chmod(config_path.c_str(), 0644) == 0, "secures source config");
		ok &= expect(chmod(models_dir.c_str(), 0755) == 0, "secures source models directory");

		if (acl_functional) {
			const auto before_success = runtime_dirs_for_uid(runtime_root, target_uid);
			const auto prepared =
			    prepare_runtime_auth_files_for_test("alice", identity, source_paths, owner_uid);
			ok &=
			    expect(prepared.has_value(), "missing source user model still prepares auth files");
			if (prepared.has_value()) {
				const auto prefix = "pam-" + std::to_string(target_uid) + "-";
				ok &= expect(prepared->runtime_dir.parent_path() == runtime_root,
				             "prepared runtime directory uses injected runtime root");
				ok &= expect(prepared->runtime_dir.filename().string().starts_with(prefix),
				             "prepared runtime directory uses pam uid prefix");
				ok &= expect(prepared->config_path == prepared->runtime_dir / "config.ini",
				             "prepared config path uses runtime directory");
				ok &= expect(prepared->user_models_dir == prepared->runtime_dir / "models",
				             "prepared models path uses runtime directory");
				ok &= expect(fs::is_directory(prepared->runtime_dir),
				             "successful prepare creates runtime directory");
				ok &= expect(read_file(prepared->config_path) == "config-content",
				             "successful prepare stages config.ini");
				ok &= expect(fs::is_directory(prepared->user_models_dir),
				             "successful prepare creates models directory");
				ok &= expect(!fs::exists(prepared->user_models_dir / "alice.dat"),
				             "missing source user model remains unstaged");
				ok &= expect(chmod(prepared->user_models_dir.c_str(), 0700) == 0,
				             "reopens successful prepared models directory for cleanup");
				ok &= expect(chmod(prepared->runtime_dir.c_str(), 0700) == 0,
				             "reopens successful prepared runtime directory for cleanup");
				fs::remove_all(prepared->runtime_dir, ec);
				ok &= expect(!ec, "cleans successful prepared runtime directory");
			}
			ok &= expect(runtime_dirs_for_uid(runtime_root, target_uid) == before_success,
			             "successful prepare fixture leaves no runtime directory");
		}

		const auto before_config_failure = runtime_dirs_for_uid(runtime_root, target_uid);
		ok &= expect(
		    !prepare_runtime_auth_files_for_test("alice", identity,
		                                         {.runtime_root  = runtime_root,
		                                          .source_config = source_dir / "missing.ini",
		                                          .source_user_models_dir = models_dir},
		                                         owner_uid)
		         .has_value(),
		    "failed config staging rejects prepare");
		ok &= expect(runtime_dirs_for_uid(runtime_root, target_uid) == before_config_failure,
		             "failed config staging removes private runtime directory");

		if (geteuid() == 0) {
			std::cerr << "SKIP: config copy permission failure requires non-root test process\n";
		} else {
			ok &= expect(chmod(config_path.c_str(), 0000) == 0, "makes source config unreadable");
			const auto before_config_copy_failure = runtime_dirs_for_uid(runtime_root, target_uid);
			ok &= expect(
			    !prepare_runtime_auth_files_for_test("alice", identity, source_paths, owner_uid)
			         .has_value(),
			    "failed config copy rejects prepare");
			ok &=
			    expect(runtime_dirs_for_uid(runtime_root, target_uid) == before_config_copy_failure,
			           "failed config copy removes private runtime directory");
			ok &= expect(chmod(config_path.c_str(), 0644) == 0, "restores source config");
		}

		if (acl_functional) {
			ok &= expect(write_file(model_path, "model-content"), "writes secure source model");
			ok &= expect(chmod(model_path.c_str(), 0644) == 0, "secures source model");
			const auto before_model_success = runtime_dirs_for_uid(runtime_root, target_uid);
			const auto prepared_with_model =
			    prepare_runtime_auth_files_for_test("alice", identity, source_paths, owner_uid);
			ok &=
			    expect(prepared_with_model.has_value(), "secure source model prepares auth files");
			if (prepared_with_model.has_value()) {
				ok &= expect(read_file(prepared_with_model->user_models_dir / "alice.dat") ==
				                 "model-content",
				             "successful prepare stages user model");
				ok &= expect(chmod(prepared_with_model->user_models_dir.c_str(), 0700) == 0,
				             "reopens prepared models directory with model for cleanup");
				ok &= expect(chmod(prepared_with_model->runtime_dir.c_str(), 0700) == 0,
				             "reopens prepared runtime directory with model for cleanup");
				fs::remove_all(prepared_with_model->runtime_dir, ec);
				ok &= expect(!ec, "cleans prepared runtime directory with model");
			}
			ok &= expect(runtime_dirs_for_uid(runtime_root, target_uid) == before_model_success,
			             "successful model prepare fixture leaves no runtime directory");

			ok &= expect(write_file(model_path, "model-content"), "writes insecure source model");
			ok &= expect(chmod(model_path.c_str(), 0664) == 0, "makes source model insecure");
			const auto before_model_failure = runtime_dirs_for_uid(runtime_root, target_uid);
			ok &= expect(
			    !prepare_runtime_auth_files_for_test("alice", identity, source_paths, owner_uid)
			         .has_value(),
			    "insecure source model rejects prepare");
			ok &= expect(runtime_dirs_for_uid(runtime_root, target_uid) == before_model_failure,
			             "insecure source model failure removes private runtime directory");
		}

		fs::current_path(original_dir, ec);
		ok &= expect(!ec, "restores test working directory");
		fs::remove_all(fixture_dir, ec);
		ok &= expect(!ec, "cleans prepare runtime auth files fixtures");
		return ok;
	}

	auto expect_wrong_owner_config_rejected(const std::filesystem::path &temp_root) -> bool {
		namespace fs = std::filesystem;
		using howdy::native::auth_helper::prepare_runtime_auth_files_for_test;

		if (geteuid() != 0) {
			std::cerr << "SKIP: wrong-owner config fixture requires chown capability\n";
			return true;
		}

		bool            ok = true;
		std::error_code ec;
		const auto      fixture_dir  = temp_root / "wrong-owner-config";
		const auto      runtime_root = fixture_dir / "runtime-root";
		const auto      source_dir   = fixture_dir / "source";
		const auto      config_path  = source_dir / "config.ini";
		const auto      models_dir   = source_dir / "models";
		fs::create_directories(runtime_root, ec);
		ok &= expect(!ec, "creates wrong-owner runtime root");
		ec.clear();
		fs::create_directories(models_dir, ec);
		ok &= expect(!ec, "creates wrong-owner models directory");
		ok &= expect(write_file(config_path, "config-content"), "writes wrong-owner source config");
		ok &=
		    expect(chmod(fixture_dir.c_str(), 0755) == 0, "secures wrong-owner fixture directory");
		ok &= expect(chmod(source_dir.c_str(), 0755) == 0, "secures wrong-owner source directory");
		ok &= expect(chmod(models_dir.c_str(), 0755) == 0, "secures wrong-owner models directory");
		ok &= expect(chmod(config_path.c_str(), 0644) == 0, "secures wrong-owner source config");
		if (chown(config_path.c_str(), 61006, 0) != 0) {
			std::cerr << "SKIP: wrong-owner config fixture chown failed: " << std::strerror(errno)
			          << "\n";
			fs::remove_all(fixture_dir, ec);
			return ok;
		}
		const auto before_prepare = runtime_dirs_for_uid(runtime_root, getuid());
		const auto prepared =
		    prepare_runtime_auth_files_for_test("alice", {.uid = getuid(), .gid = getgid()},
		                                        {.runtime_root           = runtime_root,
		                                         .source_config          = config_path,
		                                         .source_user_models_dir = models_dir},
		                                        0);
		ok &= expect(!prepared.has_value(), "wrong-owner config rejects runtime preparation");
		ok &= expect(runtime_dirs_for_uid(runtime_root, getuid()) == before_prepare,
		             "wrong-owner config removes partial runtime tree");
		fs::remove_all(fixture_dir, ec);
		ok &= expect(!ec, "cleans wrong-owner config fixtures");
		return ok;
	}

	auto expect_acl_setup_failure(const std::filesystem::path &temp_root) -> bool {
		namespace fs = std::filesystem;
		using howdy::native::auth_helper::prepare_runtime_auth_files_for_test;
		using howdy::native::auth_helper::set_acl_setup_failure_for_test;

		bool            ok = true;
		std::error_code ec;
		const auto      fixture_dir  = temp_root / "acl-setup-failure";
		const auto      runtime_root = fixture_dir / "runtime-root";
		const auto      source_dir   = fixture_dir / "source";
		const auto      original_dir = fs::current_path();
		const auto      config_path  = fs::path("./config.ini");
		const auto      models_dir   = fs::path("./models");
		const uid_t     target_uid   = geteuid() == 0 ? 61001 : geteuid();

		fs::create_directories(runtime_root, ec);
		ok &= expect(!ec, "creates ACL failure runtime root");
		ec.clear();
		fs::create_directories(source_dir / "models", ec);
		ok &= expect(!ec, "creates ACL failure source models directory");
		fs::current_path(source_dir, ec);
		ok &= expect(!ec, "uses relative ACL failure source paths");
		ok &= expect(write_file(config_path, "config-content"), "writes ACL failure source config");
		ok &= expect(write_file(models_dir / "alice.dat", "model-content"),
		             "writes ACL failure source model");
		ok &=
		    expect(chmod(fixture_dir.c_str(), 0755) == 0, "secures ACL failure fixture directory");
		ok &= expect(chmod(source_dir.c_str(), 0755) == 0, "secures ACL failure source directory");
		ok &= expect(chmod(config_path.c_str(), 0644) == 0, "secures ACL failure source config");
		ok &= expect(chmod(models_dir.c_str(), 0755) == 0,
		             "secures ACL failure source models directory");
		ok &= expect(chmod((models_dir / "alice.dat").c_str(), 0644) == 0,
		             "secures ACL failure source model");

		set_acl_setup_failure_for_test(true);
		const auto         before_failure = runtime_dirs_for_uid(runtime_root, target_uid);
		std::ostringstream failure_output;
		auto              *previous_cerr = std::cerr.rdbuf(failure_output.rdbuf());
		const auto         failed_prepare =
		    prepare_runtime_auth_files_for_test("alice", {.uid = target_uid, .gid = getegid()},
		                                        {.runtime_root           = runtime_root,
		                                         .source_config          = config_path,
		                                         .source_user_models_dir = models_dir},
		                                        geteuid());
		std::cerr.rdbuf(previous_cerr);
		set_acl_setup_failure_for_test(false);
		ok &= expect(!failed_prepare.has_value(), "ACL setup failure rejects preparation");
		ok &= expect(failure_output.str().contains("apply ACL to staged object"),
		             "ACL setup failure logs operation");
		ok &= expect(failure_output.str().contains(runtime_root.string()),
		             "ACL setup failure logs staged path");
		ok &= expect(failure_output.str().contains(std::strerror(EIO)),
		             "ACL setup failure logs saved errno");
		ok &= expect(runtime_dirs_for_uid(runtime_root, target_uid) == before_failure,
		             "ACL setup failure removes partial runtime directory");

		using howdy::native::auth_helper::set_acl_verification_failure_for_test;
		set_acl_verification_failure_for_test(true);
		const auto         before_verify_failure = runtime_dirs_for_uid(runtime_root, target_uid);
		std::ostringstream verify_failure_output;
		previous_cerr = std::cerr.rdbuf(verify_failure_output.rdbuf());
		const auto verify_failed_prepare =
		    prepare_runtime_auth_files_for_test("alice", {.uid = target_uid, .gid = getegid()},
		                                        {.runtime_root           = runtime_root,
		                                         .source_config          = config_path,
		                                         .source_user_models_dir = models_dir},
		                                        geteuid());
		std::cerr.rdbuf(previous_cerr);
		set_acl_verification_failure_for_test(false);
		ok &= expect(!verify_failed_prepare.has_value(), "ACL policy mismatch rejects preparation");
		ok &= expect(verify_failure_output.str().contains("ACL policy mismatch"),
		             "ACL policy mismatch logs policy diagnostic");
		ok &= expect(verify_failure_output.str().contains(runtime_root.string()),
		             "ACL policy mismatch logs staged path");
		ok &= expect(!verify_failure_output.str().contains("Success"),
		             "ACL policy mismatch does not log misleading Success errno");
		ok &= expect(runtime_dirs_for_uid(runtime_root, target_uid) == before_verify_failure,
		             "ACL policy mismatch removes partial runtime directory");

		fs::current_path(original_dir, ec);
		ok &= expect(!ec, "restores ACL failure test working directory");
		fs::remove_all(fixture_dir, ec);
		ok &= expect(!ec, "cleans ACL failure fixtures");
		return ok;
	}

	auto expect_staged_acl_confidentiality(const std::filesystem::path &temp_root) -> bool {
		namespace fs = std::filesystem;
		using howdy::native::auth_helper::prepare_runtime_auth_files_for_test;

		bool            ok = true;
		std::error_code ec;
		const auto      fixture_dir   = temp_root / "staged-acl-confidentiality";
		const auto      runtime_root  = fixture_dir / "runtime-root";
		const auto      source_dir    = fixture_dir / "source";
		const auto      original_dir  = fs::current_path();
		const auto      config_path   = fs::path("./config.ini");
		const auto      models_dir    = fs::path("./models");
		const uid_t     target_uid    = geteuid() == 0 ? 61001 : geteuid();
		const uid_t     shared_uid    = 61002;
		const uid_t     unrelated_uid = 61003;
		const gid_t     shared_gid    = 61004;
		const gid_t     unrelated_gid = 61005;
		const uid_t     owner_uid     = geteuid();
		const gid_t     owner_gid     = getegid();

		fs::create_directories(runtime_root, ec);
		ok &= expect(!ec, "creates ACL runtime root");
		ec.clear();
		fs::create_directories(source_dir / "models", ec);
		ok &= expect(!ec, "creates ACL source models directory");
		fs::current_path(source_dir, ec);
		ok &= expect(!ec, "uses relative ACL source paths");
		ok &= expect(write_file(config_path, "config-content"), "writes ACL source config");
		ok &= expect(write_file(models_dir / "alice.dat", "model-content"),
		             "writes ACL source model");
		ok &= expect(chmod(fixture_dir.c_str(), 0755) == 0, "secures ACL fixture directory");
		ok &= expect(chmod(source_dir.c_str(), 0755) == 0, "secures ACL source directory");
		ok &= expect(chmod(config_path.c_str(), 0644) == 0, "secures ACL source config");
		ok &= expect(chmod(models_dir.c_str(), 0755) == 0, "secures ACL source models directory");
		ok &= expect(chmod((models_dir / "alice.dat").c_str(), 0644) == 0,
		             "secures ACL source model");

		const auto before_prepare = runtime_dirs_for_uid(runtime_root, target_uid);
		const auto prepared =
		    prepare_runtime_auth_files_for_test("alice", {.uid = target_uid, .gid = shared_gid},
		                                        {.runtime_root           = runtime_root,
		                                         .source_config          = config_path,
		                                         .source_user_models_dir = models_dir},
		                                        owner_uid);
		ok &= expect(prepared.has_value(), "ACL runtime preparation succeeds");
		if (prepared.has_value()) {
			for (const auto &[path, directory, label] : {
			         std::tuple{prepared->runtime_dir, true, "runtime directory"},
			         std::tuple{prepared->user_models_dir, true, "models directory"},
			         std::tuple{prepared->config_path, false, "config file"},
			         std::tuple{prepared->user_models_dir / "alice.dat", false, "model file"},
			     }) {
				struct stat stat_{};
				ok &= expect(lstat(path.c_str(), &stat_) == 0, std::string("stats ") + label);
				ok &= expect(stat_.st_uid == owner_uid && stat_.st_gid == owner_gid,
				             std::string(label) + " remains expected privileged owner");
				ok &= expect_private_acl(path, target_uid, directory, label);
			}
			if (geteuid() == 0) {
				ok &= expect(child_can_access_staged_files(*prepared, target_uid, shared_gid, true),
				             "named target UID can traverse and read staged files");
				ok &=
				    expect(child_can_access_staged_files(*prepared, shared_uid, shared_gid, false),
				           "same primary GID UID cannot traverse or read staged files");
				ok &= expect(
				    child_can_access_staged_files(*prepared, unrelated_uid, unrelated_gid, false),
				    "unrelated UID cannot traverse or read staged files");
			} else {
				std::cerr << "SKIP: cross-UID access checks require root\n";
			}
			ok &= expect(chmod(prepared->user_models_dir.c_str(), 0700) == 0,
			             "reopens ACL prepared models directory for cleanup");
			ok &= expect(chmod(prepared->runtime_dir.c_str(), 0700) == 0,
			             "reopens ACL prepared runtime directory for cleanup");
			fs::remove_all(prepared->runtime_dir, ec);
			ok &= expect(!ec, "cleans ACL prepared runtime directory");
		}
		ok &= expect(runtime_dirs_for_uid(runtime_root, target_uid) == before_prepare,
		             "ACL prepared runtime directory is removed");

		fs::current_path(original_dir, ec);
		ok &= expect(!ec, "restores ACL test working directory");
		fs::remove_all(fixture_dir, ec);
		ok &= expect(!ec, "cleans ACL staging fixtures");
		return ok;
	}

	auto expect_stdout_protocol() -> bool {
		using namespace howdy::native::auth_helper_protocol;
		using howdy::native::testing::print_prepared_paths;

		bool ok = true;
		ok &= expect(std::string(kConfigPathKey) == "CONFIG_PATH",
		             "config path protocol key remains unchanged");
		ok &= expect(std::string(kUserModelsDirKey) == "USER_MODELS_DIR",
		             "user models directory protocol key remains unchanged");

		std::ostringstream output;
		auto              *previous = std::cout.rdbuf(output.rdbuf());
		print_prepared_paths("/run/howdy/pam-1000-example/config.ini",
		                     "/run/howdy/pam-1000-example/models");
		std::cout.rdbuf(previous);

		ok &= expect(output.str() == "CONFIG_PATH=/run/howdy/pam-1000-example/config.ini\n"
		                             "USER_MODELS_DIR=/run/howdy/pam-1000-example/models\n",
		             "prepare stdout protocol remains unchanged");
		return ok;
	}

}  // namespace

auto main() -> int {
	namespace fs = std::filesystem;

	bool       ok = true;
	const auto temp_root =
	    fs::current_path() / ("howdy-auth-helper-test-" + std::to_string(getpid()));
	std::error_code ec;
	fs::remove_all(temp_root, ec);
	fs::create_directories(temp_root, ec);
	ok &= expect(!ec, "creates auth-helper temp root");
	const auto acl_support_result = acl_support(temp_root);
	ok &= expect_acl_probe_classification(temp_root);
	if (acl_support_result == AclSupport::kUnsupported) {
		std::cerr << "SKIP: filesystem does not support usable POSIX named-user ACLs\n";
	} else if (acl_support_result == AclSupport::kError) {
		ok = false;
	}
	const bool  acl_supported  = acl_support_result == AclSupport::kSupported;
	const bool  acl_functional = acl_support_result != AclSupport::kError;
	const bool  use_fake_acl   = acl_support_result == AclSupport::kUnsupported;
	const uid_t functional_target_uid =
	    use_fake_acl && geteuid() == 0 && getuid() == geteuid() ? 61001 : getuid();
	if (use_fake_acl) {
		install_fake_acl_backend();
	}

	ok &= expect_runtime_root_validation(temp_root);
	ok &= expect_secure_source_file_stat(temp_root);
	ok &= expect_write_all_helper(temp_root);
	ok &= expect_fake_acl_backend(temp_root);
	if (acl_supported && geteuid() == 0 && getuid() == geteuid()) {
		constexpr uid_t kDistinctTargetUid = 61001;
		install_fake_acl_backend();
		ok &= expect_prepare_runtime_auth_files(temp_root, true, kDistinctTargetUid);
		ok &= expect_fake_acl_activity("distinct-target fake ACL preparation");
		ok &= expect_fake_acl_target(kDistinctTargetUid, geteuid());
		ok &= reset_fake_acl_backend();
	}
	if (use_fake_acl) {
		install_fake_acl_backend();
	}
	ok &= expect_copy_file_for_user(temp_root, acl_functional, acl_supported);
	if (use_fake_acl) {
		ok &= expect_fake_acl_activity("copy_file_for_user");
		ok &= reset_fake_acl_backend();
		install_fake_acl_backend();
	}
	ok &= expect_source_model_readiness(temp_root, acl_functional);
	if (use_fake_acl) {
		ok &= expect_fake_acl_activity("source_model_readiness");
		ok &= reset_fake_acl_backend();
		install_fake_acl_backend();
	}
	ok &= expect_prepare_cleanup_guards();
	ok &= expect_cleanup_runtime_auth_files(temp_root);
	ok &= expect_wrong_owner_config_rejected(temp_root);
	ok &= expect_prepare_runtime_auth_files(temp_root, acl_functional, functional_target_uid);
	if (use_fake_acl) {
		ok &= expect_fake_acl_activity("prepare_runtime_auth_files");
		ok &= expect_fake_acl_target(functional_target_uid, geteuid());
		ok &= reset_fake_acl_backend();
		install_fake_acl_backend();
	}
	ok &= expect_acl_setup_failure(temp_root);
	if (use_fake_acl) {
		ok &= reset_fake_acl_backend();
	}
	if (acl_supported) {
		ok &= expect_staged_acl_confidentiality(temp_root);
	}
	ok &= expect_stdout_protocol();

	fs::remove_all(temp_root, ec);
	return ok ? 0 : 1;
}
