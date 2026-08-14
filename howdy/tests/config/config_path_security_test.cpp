#include "config/config_utils.hpp"
#include "config/config_utils_test_support.hpp"
#include "config/config_validation.hpp"
#include "test_support.hpp"

#include <cerrno>
#include <filesystem>
#include <string>
#include <unistd.h>

#include <sys/resource.h>
#include <sys/stat.h>

namespace howdy::test {
	namespace {
		auto lock_path_for_config(const std::filesystem::path &config_path)
		    -> std::filesystem::path {
			return config_path.string() + ".lock";
		}

	}  // namespace

	auto run_config_lock_failure_tests(ConfigUtilsTestContext &context) -> bool {
		namespace fs = std::filesystem;
		using howdy::test::expect;
		bool            ok = true;
		std::error_code ec;
		const auto      lock_failure_path = lock_path_for_config(context.config_path);
		fs::remove(lock_failure_path, ec);
		ec.clear();
		fs::create_symlink("/tmp", lock_failure_path, ec);
		ok &= expect(!ec, "create config lock symlink for lock failure");
		ok &= expect(howdy::native::read_config_lines(context.config_path, true).empty(),
		             "read_config_lines fails closed when lock cannot be opened");
		std::string lock_error;
		ok &= expect(!howdy::native::update_config_value(context.config_path, "disabled",
		                                                 &lock_error, "false", true, false) &&
		                 lock_error == "Failed to lock config file",
		             "update_config_value reports config lock failure");
		fs::remove(lock_failure_path, ec);
		ec.clear();

		return ok;
	}

	auto run_config_path_security_tests(ConfigUtilsTestContext &context) -> bool {
		namespace fs = std::filesystem;
		using howdy::test::expect;
		bool            ok = true;
		std::error_code ec;
		const auto      install_failure_dir = context.temp_root / "install-failure";
		ok &= expect(fs::create_directories(install_failure_dir, ec) || !ec,
		             "create install-failure directory");
		ok &= expect(!ec, "no error creating install-failure directory");
		const auto install_failure_path = install_failure_dir / "config.ini";
		ok &= expect(write_config_test_file(install_failure_path, "[core]\ndisabled = false\n"),
		             "write install-failure config");
		std::string update_error;
		bool        update_failed = false;
		const auto  install_failure_security =
		    howdy::native::check_secure_config_path(install_failure_path);
		ok &= expect(install_failure_security.ok,
		             "install-failure config passes secure path check before update");
		FileSizeLimitGuard file_size_limit_guard;
		const bool         have_file_size_limit = file_size_limit_guard.have_original;
		ok &= expect(have_file_size_limit, "read file-size limit for install-failure test");
		const bool set_file_size_limit = file_size_limit_guard.set_zero();
		ok &= expect(set_file_size_limit, "set file-size limit for install-failure test");
		if (install_failure_security.ok && set_file_size_limit) {
			update_failed = !howdy::native::update_config_value(
			    install_failure_path, "disabled", &update_error, "true", false, false);
		}
		if (set_file_size_limit) {
			ok &= expect(file_size_limit_guard.restore(),
			             "restore file-size limit after install-failure test");
		}
		ok &= expect(update_failed, "update_config_value returns false when install fails");
		ok &= expect(update_error == "Failed to update config file",
		             "failed update_config_value install reports fallback error: " + update_error);

		const auto config_lock_path = lock_path_for_config(context.config_path);
		fs::remove(config_lock_path, ec);
		ok &= expect(!ec && !fs::exists(config_lock_path),
		             "config lock absent before insecure config update");
		ok &= expect(chmod(context.config_path.c_str(), 0666) == 0,
		             "make config file world-writable");
		ok &= expect(!howdy::native::update_config_value(context.config_path, "disabled", nullptr,
		                                                 "false", true),
		             "update_config_value rejects insecure config permissions");
		ok &= expect(!fs::exists(config_lock_path),
		             "update_config_value rejects insecure config before creating lock");
		ok &= expect(chmod(context.config_path.c_str(), 0644) == 0, "restore config permissions");

		const auto insecure_dir = context.temp_root / "insecure-dir";
		ok &= expect(fs::create_directories(insecure_dir, ec) || !ec, "create insecure config dir");
		ok &= expect(!ec, "no error creating insecure config dir");
		const auto insecure_config_path = insecure_dir / "config.ini";
		ok &= expect(write_config_test_file(insecure_config_path, "[core]\ndisabled = false\n"),
		             "write config in insecure dir");
		const auto insecure_config_lock_path = lock_path_for_config(insecure_config_path);
		fs::remove(insecure_config_lock_path, ec);
		ok &= expect(!ec && !fs::exists(insecure_config_lock_path),
		             "config lock absent before insecure directory update");
		ok &= expect(chmod(insecure_dir.c_str(), 0777) == 0, "make config dir world-writable");
		ok &= expect(!howdy::native::check_secure_config_path(insecure_config_path).ok,
		             "check_secure_config_path rejects insecure config directory");
		ok &= expect(!howdy::native::update_config_value(insecure_config_path, "disabled", nullptr,
		                                                 "true", true),
		             "update_config_value rejects insecure config directory");
		ok &= expect(!fs::exists(insecure_config_lock_path),
		             "update_config_value rejects insecure directory before creating lock");
		ok &= expect(chmod(insecure_dir.c_str(), 0755) == 0, "restore config dir mode");

		const auto insecure_ancestor_root = context.temp_root / "insecure-ancestor";
		const auto nested_config_dir      = insecure_ancestor_root / "nested" / "deeper";
		ok &= expect(fs::create_directories(nested_config_dir, ec) || !ec,
		             "create nested config dir");
		ok &= expect(!ec, "no error creating nested config dir");
		const auto nested_config_path = nested_config_dir / "config.ini";
		ok &= expect(write_config_test_file(nested_config_path, "[core]\ndisabled = false\n"),
		             "write config in nested dir");
		const auto nested_config_lock_path = lock_path_for_config(nested_config_path);
		fs::remove(nested_config_lock_path, ec);
		ok &= expect(!ec && !fs::exists(nested_config_lock_path),
		             "config lock absent before insecure ancestor update");
		ok &= expect(chmod(insecure_ancestor_root.c_str(), 0777) == 0,
		             "make ancestor config dir world-writable");
		ok &= expect(!howdy::native::check_secure_config_path(nested_config_path).ok,
		             "check_secure_config_path rejects insecure ancestor directory");
		ok &= expect(!howdy::native::update_config_value(nested_config_path, "disabled", nullptr,
		                                                 "true", true),
		             "update_config_value rejects insecure ancestor directory");
		ok &= expect(!fs::exists(nested_config_lock_path),
		             "update_config_value rejects insecure ancestor before creating lock");
		ok &= expect(chmod(insecure_ancestor_root.c_str(), 0755) == 0,
		             "restore ancestor config dir mode");

		const auto unreadable_dir = context.temp_root / "unreadable-dir";
		ok &= expect(fs::create_directories(unreadable_dir, ec) || !ec,
		             "create unreadable config dir");
		ok &= expect(!ec, "no error creating unreadable config dir");
		const auto unreadable_config_path = unreadable_dir / "config.ini";
		ok &= expect(write_config_test_file(unreadable_config_path, "[core]\ndisabled = false\n"),
		             "write config in unreadable dir");
		ok &= expect(chmod(unreadable_dir.c_str(), 0000) == 0, "make config dir unreadable");
		if (geteuid() != 0) {
			const auto unreadable_check =
			    howdy::native::check_secure_config_path(unreadable_config_path);
			ok &= expect(!unreadable_check.ok,
			             "check_secure_config_path rejects inaccessible config path");
			ok &= expect(unreadable_check.error_code == EACCES,
			             "inaccessible config path preserves EACCES");
			ok &= expect(unreadable_check.error_message.contains("process uid="),
			             "inaccessible config path reports process uid");
			ok &= expect(unreadable_check.error_message.contains("do not make /etc/howdy"),
			             "inaccessible config path warns against insecure permissions");
		}
		ok &=
		    expect(chmod(unreadable_dir.c_str(), 0755) == 0, "restore unreadable config dir mode");

		const auto protected_path = context.temp_root / "protected.ini";
		ok &= expect(write_config_test_file(protected_path, "[core]\ndisabled = false\n"),
		             "write protected config");
		ok &= expect(chmod(protected_path.c_str(), 0600) == 0, "set protected config mode");
		const auto strict_root_check =
		    howdy::native::check_secure_config_path(protected_path, static_cast<uid_t>(0));
		if (geteuid() != 0) {
			ok &= expect(!strict_root_check.ok,
			             "strict root-owned config check rejects non-root-owned config");
			ok &= expect(strict_root_check.error_message.contains("owned by UID 0"),
			             "strict root-owned config check reports root ownership requirement");
		} else {
			ok &= expect(strict_root_check.ok,
			             "strict root-owned config check accepts root-owned config");
		}
		ok &= expect(howdy::native::update_config_value(protected_path, "disabled", "true"),
		             "update_config_value succeeds on secure config");
		struct stat protected_stat{};
		ok &= expect(stat(protected_path.c_str(), &protected_stat) == 0, "stat protected config");
		ok &= expect((protected_stat.st_mode & 0777) == 0600, "atomic write preserves config mode");

		const auto hardlink_path = context.temp_root / "protected-hardlink.ini";
		ok &= expect(link(protected_path.c_str(), hardlink_path.c_str()) == 0,
		             "create hard link to protected config");
		ok &= expect(!howdy::native::update_config_value(hardlink_path, "disabled", "false"),
		             "update_config_value rejects hard-linked config");

		howdy::native::ConfigReader validated(context.config_path.string());
		ok &= expect(validated.ok(), "validated config still parses");
		ok &= expect(!howdy::native::validate_runtime_config(validated).has_value(),
		             "validated config passes semantic validation");
		return ok;
	}

}  // namespace howdy::test
