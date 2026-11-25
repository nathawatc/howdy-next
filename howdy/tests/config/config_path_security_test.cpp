#include "../../src/config/config_reader/internal.hpp"
#include "../../src/config/config_validation/internal.hpp"
#include "config/config_utils.hpp"
#include "config/config_utils_test_support.hpp"
#include "support/bit_flags.hpp"
#include "support/file_lock.hpp"
#include "support/file_security.hpp"
#include "support/scoped_fd.hpp"
#include "test_support.hpp"

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace howdy::test {
	using howdy::native::LockFilePath;

	namespace {

		auto ProcLocksLists(std::string_view kind, ino_t inode) -> bool {
			const auto    suffix = ":" + std::to_string(inode) + " ";
			std::ifstream locks("/proc/locks");
			for (std::string line; std::getline(locks, line);) {
				if (line.contains(kind) && line.contains(suffix)) {
					return true;
				}
			}
			return false;
		}

		// Waits until another thread's flock() on `inode` is queued behind the current holder.
		auto WaitForBlockedFlock(ino_t inode) -> bool {
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (std::chrono::steady_clock::now() < deadline) {
				if (ProcLocksLists("-> FLOCK", inode)) {
					return true;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			return false;
		}

		auto ExpectLockReplacedWhileWaitingFails(const ConfigUtilsTestContext &context,
		                                         bool                          recreate) -> bool {
			const auto             lock_path = LockFilePath(context.config_path);
			const auto             before    = ReadFile(context.config_path);
			native::ScopedFd const holder(
			    open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
			struct stat lock_stat{};
			bool        ok = Expect(holder.Valid() && fstat(holder.Get(), &lock_stat) == 0 &&
			                            flock(holder.Get(), LOCK_EX) == 0,
			                        "hold config lock");
			if (ok && !ProcLocksLists("FLOCK", lock_stat.st_ino)) {
				std::cerr << "SKIP: /proc/locks does not list the held config lock\n";
				std::error_code ec;
				std::filesystem::remove(lock_path, ec);
				return true;
			}
			auto waiting = std::async(std::launch::async, [&] -> native::ConfigWriteResult {
				return native::UpdateConfigValue(
				    context.config_path, {.key = "disabled", .value = "true"},
				    {.validate_runtime = false, .validation_root = {context.temp_root}});
			});
			ok &= Expect(WaitForBlockedFlock(lock_stat.st_ino), "config writer waits for the lock");
			ok &= Expect(unlink(lock_path.c_str()) == 0, "remove config lock while held");
			native::ScopedFd replacement;
			if (recreate) {
				replacement.Reset(open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
				ok &= Expect(replacement.Valid(), "recreate config lock while held");
			}
			ok &= Expect(flock(holder.Get(), LOCK_UN) == 0, "release removed config lock");
			const auto result = waiting.get();
			ok &=
			    Expect(result.status == native::ConfigWriteStatus::kLockFailed &&
				           result.message.contains("removed or replaced"),
				       "config writer rejects a lock replaced while it waited: " + result.message);
			ok &= Expect(ReadFile(context.config_path) == before,
			             "rejected config writer leaves the config unchanged");
			std::error_code ec;
			std::filesystem::remove(lock_path, ec);
			return ok;
		}

	}  // namespace

	auto RunConfigLockFailureTests(ConfigUtilsTestContext &context) -> bool {
		namespace fs = std::filesystem;
		using howdy::test::Expect;
		bool            ok = true;
		std::error_code ec;
		const auto      lock_failure_path = LockFilePath(context.config_path);
		fs::remove(lock_failure_path, ec);
		ec.clear();
		fs::create_symlink("/tmp", lock_failure_path, ec);
		ok &= Expect(!ec, "create config lock symlink for lock failure");
		auto lock_result = howdy::native::UpdateConfigValue(
		    context.config_path, {.key = "disabled", .value = "false"},
		    {.validate_runtime = false, .validation_root = {context.temp_root}});
		ok &= Expect(lock_result.status == howdy::native::ConfigWriteStatus::kLockFailed &&
		                 lock_result.message.starts_with("Failed to lock config file: "),
		             "update_config_value reports config lock failure with a reason: " +
		                 lock_result.message);
		fs::remove(lock_failure_path, ec);
		ec.clear();

		ok &= Expect(WriteConfigTestFile(lock_failure_path, ""),
		             "create pre-existing lock file with relaxed permissions");
		ok &= Expect(chmod(lock_failure_path.c_str(), 0644) == 0,
		             "set config lock file group/world-readable");
		lock_result = howdy::native::UpdateConfigValue(
		    context.config_path, {.key = "disabled", .value = "false"},
		    {.validate_runtime = false, .validation_root = {context.temp_root}});
		ok &= Expect(lock_result.status == howdy::native::ConfigWriteStatus::kLockFailed &&
		                 lock_result.message.starts_with("Failed to lock config file: ") &&
		                 lock_result.message.contains("mode 0644, expected 0600"),
		             "update_config_value rejects pre-existing 0644 lock file with a mode hint: " +
		                 lock_result.message);
		ok &= Expect(chmod(lock_failure_path.c_str(), 0664) == 0,
		             "set config lock file group-writable");
		lock_result = howdy::native::UpdateConfigValue(
		    context.config_path, {.key = "disabled", .value = "false"},
		    {.validate_runtime = false, .validation_root = {context.temp_root}});
		ok &= Expect(lock_result.status == howdy::native::ConfigWriteStatus::kLockFailed &&
		                 lock_result.message.contains("mode 0664, expected 0600; remove "),
		             "update_config_value gives the same hint for a writable lock file: " +
		                 lock_result.message);
		fs::remove(lock_failure_path, ec);
		ec.clear();
		ok &= ExpectLockReplacedWhileWaitingFails(context, false);
		ok &= ExpectLockReplacedWhileWaitingFails(context, true);

		return ok;
	}

	auto RunConfigPathSecurityTests(ConfigUtilsTestContext &context) -> bool {
		namespace fs = std::filesystem;
		using howdy::test::Expect;
		bool            ok = true;
		std::error_code ec;
		const auto      install_failure_dir = context.temp_root / "install-failure";
		ok &= Expect(fs::create_directories(install_failure_dir, ec) || !ec,
		             "create install-failure directory");
		ok &= Expect(!ec, "no error creating install-failure directory");
		const auto install_failure_path = install_failure_dir / "config.ini";
		ok &= Expect(WriteConfigTestFile(install_failure_path, "[core]\ndisabled = false\n"),
		             "write install-failure config");
		howdy::native::ConfigWriteResult update_result;
		bool                             update_failed = false;
		const auto install_failure_security            = howdy::native::CheckSecureConfigPath(
		    install_failure_path, howdy::native::DefaultSecureOwnerUid(), {context.temp_root});
		ok &= Expect(install_failure_security.ok,
		             "install-failure config passes secure path check before update");
		FileSizeLimitGuard file_size_limit_guard;
		const bool         have_file_size_limit = file_size_limit_guard.Available();
		ok &= Expect(have_file_size_limit, "read file-size limit for install-failure test");
		const bool set_file_size_limit = file_size_limit_guard.SetZero();
		ok &= Expect(set_file_size_limit, "set file-size limit for install-failure test");
		if (install_failure_security.ok && set_file_size_limit) {
			update_result = howdy::native::UpdateConfigValue(
			    install_failure_path, {.key = "disabled", .value = "true"},
			    {.lock = false, .validate_runtime = false, .validation_root = {context.temp_root}});
			update_failed = update_result.status != howdy::native::ConfigWriteStatus::kOk;
		}
		if (set_file_size_limit) {
			ok &= Expect(file_size_limit_guard.Restore(),
			             "restore file-size limit after install-failure test");
		}
		ok &= Expect(update_failed, "update_config_value returns false when install fails");
		ok &= Expect(update_result.status == howdy::native::ConfigWriteStatus::kInstallFailed &&
		                 update_result.message == "Failed to update config file",
		             "failed update_config_value install reports fallback error: " +
		                 update_result.message);
		ok &= Expect(ReadFile(install_failure_path) == "[core]\ndisabled = false\n",
		             "failed install leaves original config unchanged");
		ok &= Expect(CountFilesWithPrefix(install_failure_dir, ".howdy-config-") == 0,
		             "failed install leaves no staged config file");

		const auto symlink_target = install_failure_dir / "symlink-target.ini";
		const auto symlink_config = install_failure_dir / "symlink-config.ini";
		ok &= Expect(WriteConfigTestFile(symlink_target, "[core]\ndisabled = false\n"),
		             "write symlink config target");
		fs::create_symlink(symlink_target, symlink_config, ec);
		ok &= Expect(!ec, "create symlinked config path");
		const auto symlink_result = howdy::native::UpdateConfigValue(
		    symlink_config, {.key = "disabled", .value = "true"},
		    {.lock = false, .validate_runtime = false, .validation_root = {context.temp_root}});
		ok &= Expect(symlink_result.status == howdy::native::ConfigWriteStatus::kInsecurePath,
		             "update_config_value rejects symlinked config path");
		ok &= Expect(ReadFile(symlink_target) == "[core]\ndisabled = false\n",
		             "symlinked config target is left unchanged");

		const auto config_lock_path = LockFilePath(context.config_path);
		fs::remove(config_lock_path, ec);
		ok &= Expect(!ec && !fs::exists(config_lock_path),
		             "config lock absent before insecure config update");
		ok &= Expect(chmod(context.config_path.c_str(), 0666) == 0,
		             "make config file world-writable");
		ok &= Expect(howdy::native::UpdateConfigValue(context.config_path,
		                                              {.key = "disabled", .value = "false"},
		                                              {.validation_root = {context.temp_root}})
		                     .status != howdy::native::ConfigWriteStatus::kOk,
		             "update_config_value rejects insecure config permissions");
		ok &= Expect(!fs::exists(config_lock_path),
		             "update_config_value rejects insecure config before creating lock");
		ok &= Expect(chmod(context.config_path.c_str(), 0644) == 0, "restore config permissions");

		const auto insecure_dir = context.temp_root / "insecure-dir";
		ok &= Expect(fs::create_directories(insecure_dir, ec) || !ec, "create insecure config dir");
		ok &= Expect(!ec, "no error creating insecure config dir");
		const auto insecure_config_path = insecure_dir / "config.ini";
		ok &= Expect(WriteConfigTestFile(insecure_config_path, "[core]\ndisabled = false\n"),
		             "write config in insecure dir");
		const auto insecure_config_lock_path = LockFilePath(insecure_config_path);
		fs::remove(insecure_config_lock_path, ec);
		ok &= Expect(!ec && !fs::exists(insecure_config_lock_path),
		             "config lock absent before insecure directory update");
		ok &= Expect(chmod(insecure_dir.c_str(), 0777) == 0, "make config dir world-writable");
		ok &= Expect(!howdy::native::CheckSecureConfigPath(insecure_config_path,
		                                                   howdy::native::DefaultSecureOwnerUid(),
		                                                   {context.temp_root})
		                  .ok,
		             "check_secure_config_path rejects insecure config directory");
		ok &= Expect(howdy::native::UpdateConfigValue(insecure_config_path,
		                                              {.key = "disabled", .value = "true"},
		                                              {.validation_root = {context.temp_root}})
		                     .status != howdy::native::ConfigWriteStatus::kOk,
		             "update_config_value rejects insecure config directory");
		ok &= Expect(!fs::exists(insecure_config_lock_path),
		             "update_config_value rejects insecure directory before creating lock");
		ok &= Expect(chmod(insecure_dir.c_str(), 0755) == 0, "restore config dir mode");

		const auto insecure_ancestor_root = context.temp_root / "insecure-ancestor";
		const auto nested_config_dir      = insecure_ancestor_root / "nested" / "deeper";
		ok &= Expect(fs::create_directories(nested_config_dir, ec) || !ec,
		             "create nested config dir");
		ok &= Expect(!ec, "no error creating nested config dir");
		const auto nested_config_path = nested_config_dir / "config.ini";
		ok &= Expect(WriteConfigTestFile(nested_config_path, "[core]\ndisabled = false\n"),
		             "write config in nested dir");
		const auto nested_config_lock_path = LockFilePath(nested_config_path);
		fs::remove(nested_config_lock_path, ec);
		ok &= Expect(!ec && !fs::exists(nested_config_lock_path),
		             "config lock absent before insecure ancestor update");
		ok &= Expect(chmod(insecure_ancestor_root.c_str(), 0777) == 0,
		             "make ancestor config dir world-writable");
		ok &= Expect(!howdy::native::CheckSecureConfigPath(nested_config_path,
		                                                   howdy::native::DefaultSecureOwnerUid(),
		                                                   {context.temp_root})
		                  .ok,
		             "check_secure_config_path rejects insecure ancestor directory");
		ok &= Expect(howdy::native::UpdateConfigValue(nested_config_path,
		                                              {.key = "disabled", .value = "true"},
		                                              {.validation_root = {context.temp_root}})
		                     .status != howdy::native::ConfigWriteStatus::kOk,
		             "update_config_value rejects insecure ancestor directory");
		ok &= Expect(!fs::exists(nested_config_lock_path),
		             "update_config_value rejects insecure ancestor before creating lock");
		ok &= Expect(chmod(insecure_ancestor_root.c_str(), 0755) == 0,
		             "restore ancestor config dir mode");

		const auto unreadable_dir = context.temp_root / "unreadable-dir";
		ok &= Expect(fs::create_directories(unreadable_dir, ec) || !ec,
		             "create unreadable config dir");
		ok &= Expect(!ec, "no error creating unreadable config dir");
		const auto unreadable_config_path = unreadable_dir / "config.ini";
		ok &= Expect(WriteConfigTestFile(unreadable_config_path, "[core]\ndisabled = false\n"),
		             "write config in unreadable dir");
		ok &= Expect(chmod(unreadable_dir.c_str(), 0000) == 0, "make config dir unreadable");
		if (geteuid() != 0) {
			const auto unreadable_check = howdy::native::CheckSecureConfigPath(
			    unreadable_config_path, howdy::native::DefaultSecureOwnerUid(),
			    {context.temp_root});
			ok &= Expect(!unreadable_check.ok,
			             "check_secure_config_path rejects inaccessible config path");
			ok &= Expect(unreadable_check.error_code == EACCES,
			             "inaccessible config path preserves EACCES");
			ok &= Expect(unreadable_check.error_message.contains("process uid="),
			             "inaccessible config path reports process uid");
			ok &= Expect(unreadable_check.error_message.contains("do not make /etc/howdy"),
			             "inaccessible config path warns against insecure permissions");
		}
		ok &=
		    Expect(chmod(unreadable_dir.c_str(), 0755) == 0, "restore unreadable config dir mode");

		const auto protected_path = context.temp_root / "protected.ini";
		ok &= Expect(WriteConfigTestFile(protected_path, "[core]\ndisabled = false\n"),
		             "write protected config");
		ok &= Expect(chmod(protected_path.c_str(), 0600) == 0, "set protected config mode");
		const auto strict_root_check = howdy::native::CheckSecureConfigPath(
		    protected_path, static_cast<uid_t>(0), {context.temp_root});
		if (geteuid() != 0) {
			ok &= Expect(!strict_root_check.ok,
			             "strict root-owned config check rejects non-root-owned config");
			ok &= Expect(strict_root_check.error_message.contains("owned by UID 0"),
			             "strict root-owned config check reports root ownership requirement");
		} else {
			ok &= Expect(strict_root_check.ok,
			             "strict root-owned config check accepts root-owned config");
		}
		ok &= Expect(howdy::native::UpdateConfigValue(
		                 protected_path, {.key = "disabled", .value = "true"},
		                 {.lock = false, .validation_root = {context.temp_root}})
		                     .status == howdy::native::ConfigWriteStatus::kOk,
		             "update_config_value succeeds on secure config");
		struct stat protected_stat{};
		ok &= Expect(stat(protected_path.c_str(), &protected_stat) == 0, "stat protected config");
		ok &= Expect(howdy::native::MaskedBits(protected_stat.st_mode, 0777) == 0600,
		             "atomic write preserves config mode");

		const auto hardlink_path = context.temp_root / "protected-hardlink.ini";
		ok &= Expect(link(protected_path.c_str(), hardlink_path.c_str()) == 0,
		             "create hard link to protected config");
		ok &= Expect(howdy::native::UpdateConfigValue(
		                 hardlink_path, {.key = "disabled", .value = "false"},
		                 {.lock = false, .validation_root = {context.temp_root}})
		                     .status != howdy::native::ConfigWriteStatus::kOk,
		             "update_config_value rejects hard-linked config");

		howdy::native::ConfigReader const validated(ReadFile(context.config_path));
		ok &= Expect(validated.Ok(), "validated config still parses");
		ok &= Expect(!howdy::native::ValidateRuntimeConfig(validated).has_value(),
		             "validated config passes semantic validation");
		return ok;
	}

}  // namespace howdy::test
