#include "config/config_limits.hpp"
#include "config/runtime_config_loader.hpp"
#include "config/runtime_paths.hpp"
#include "config/test_hooks.hpp"
#include "support/file_security.hpp"
#include "test_support.hpp"

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

#include <sys/stat.h>

namespace {

	using howdy::test::Expect;
	using howdy::test::WriteFile;

	auto ExpectFailure(const howdy::native::RuntimeConfigLoadResult &result,
	                   howdy::native::RuntimeConfigLoadStatus status, const std::string &message)
	    -> bool {
		bool ok = true;
		ok &= Expect(result.status == status, message + " status");
		ok &= Expect(!result.ok, message + " is not ok");
		ok &= Expect(!result.config.has_value(), message + " has no config");
		ok &= Expect(!result.error_message.empty(), message + " has stable caller error");
		return ok;
	}

	struct TemporaryDirectory {
		std::filesystem::path path;

		~TemporaryDirectory() {
			std::error_code ec;
			std::filesystem::remove_all(path, ec);
		}
	};

	auto CreateTempDirectory() -> std::optional<std::filesystem::path> {
		const auto template_path =
		    std::filesystem::temp_directory_path() / "howdy-runtime-config-load-test-XXXXXX";
		const auto        template_string = template_path.string();
		std::vector<char> path_buffer(template_string.begin(), template_string.end());
		path_buffer.push_back('\0');

		char *created = mkdtemp(path_buffer.data());
		if (created == nullptr) {
			return std::nullopt;
		}
		return std::filesystem::path(created);
	}

}  // namespace

auto main() -> int {
	namespace fs = std::filesystem;
	using howdy::native::RuntimeConfigLoadStatus;

	bool ok = true;

	const auto temp_directory = CreateTempDirectory();
	if (!temp_directory.has_value()) {
		std::cerr << "FAIL: create unique temp root\n";
		return 1;
	}
	const TemporaryDirectory temp_directory_guard{.path = *temp_directory};
	const auto              &temp_root = temp_directory_guard.path;
	ok &= Expect(chmod(temp_root.c_str(), 0755) == 0, "secure temp root");

	const auto valid_path = temp_root / "valid.ini";
	ok &= Expect(WriteFile(valid_path, "[core]\ndisabled = false\n"), "write valid config");
	ok &= Expect(chmod(valid_path.c_str(), 0644) == 0, "secure valid config");
	setenv("HOWDY_CONFIG", valid_path.c_str(), 1);
	auto valid = howdy::native::LoadRuntimeConfig(
	    howdy::native::ResolveConfigPath(), howdy::native::DefaultSecureOwnerUid(), {temp_root});
	unsetenv("HOWDY_CONFIG");
	ok &= Expect(valid.ok && valid.status == RuntimeConfigLoadStatus::kOk, "valid config loads");
	ok &= Expect(valid.config.has_value(), "valid config has config");
	ok &= Expect(valid.path == valid_path, "default loader resolves config path");
	ok &= Expect(valid.error_message.empty(), "valid config has no error");
	ok &= Expect(chmod(temp_root.c_str(), 0777) == 0,
	             "make default-loader fixture ancestor insecure");
	setenv("HOWDY_CONFIG", valid_path.c_str(), 1);
	const auto default_load = howdy::native::LoadRuntimeConfig();
	unsetenv("HOWDY_CONFIG");
	ok &= ExpectFailure(default_load, RuntimeConfigLoadStatus::kPathError,
	                    "default loader retains full ancestor validation");
	ok &= Expect(default_load.path == valid_path, "default loader resolves configured path");
	ok &= Expect(chmod(temp_root.c_str(), 0755) == 0, "restore loader fixture boundary");

	const auto        large_runtime_path    = temp_root / "large-runtime.ini";
	const std::string large_runtime_content = "[core]\ndisabled = false\n[video]\ntimeout = 7\n" +
	                                          std::string(howdy::native::kMaxConfigFileSize, '\n');
	ok &= Expect(large_runtime_content.size() > howdy::native::kMaxConfigFileSize,
	             "large runtime config exceeds bounded config size");
	ok &=
	    Expect(WriteFile(large_runtime_path, large_runtime_content), "write large runtime config");
	ok &= Expect(chmod(large_runtime_path.c_str(), 0644) == 0, "secure large runtime config");
	const auto large_runtime =
	    howdy::native::LoadRuntimeConfig(large_runtime_path, std::nullopt, {temp_root});
	ok &= Expect(large_runtime.ok && large_runtime.config.has_value() &&
	                 large_runtime.config->video.timeout == 7,
	             "runtime config accepts content above bounded config size");

	const auto      dir_config_path = temp_root / "dir-config.ini";
	std::error_code ec;
	fs::create_directory(dir_config_path, ec);
	ok &= Expect(!ec, "create directory config path");
	const auto dir_result =
	    howdy::native::LoadRuntimeConfig(dir_config_path, std::nullopt, {temp_root});
	ok &= ExpectFailure(dir_result, RuntimeConfigLoadStatus::kPathError,
	                    "directory config is rejected");

	{
		const auto invariant_path = temp_root / "invariant.ini";
		ok &= Expect(WriteFile(invariant_path, "[core]\ndisabled = false\n[video]\ntimeout = 7\n"),
		             "write invariant original config");
		ok &= Expect(chmod(invariant_path.c_str(), 0644) == 0, "secure invariant config");

		bool       replacement_ok   = false;
		const auto backup_invariant = temp_root / "invariant-backup.ini";
		{
			const howdy::native::config_test_hooks::ScopedHooks hooks([&]() -> void {
				replacement_ok = rename(invariant_path.c_str(), backup_invariant.c_str()) == 0 &&
				                 symlink("/dev/null", invariant_path.c_str()) == 0;
			});

			const auto invariant_result =
			    howdy::native::LoadRuntimeConfig(invariant_path, std::nullopt, {temp_root});
			ok &= Expect(replacement_ok, "invariant pathname replacement succeeded");
			ok &= Expect(invariant_result.ok,
			             "load succeeds using already opened and validated descriptor");
			if (invariant_result.config.has_value()) {
				ok &= Expect(invariant_result.config->video.timeout == 7,
				             "consumed object is the opened descriptor (timeout 7), not "
				             "replaced pathname symlink");
				ok &= Expect(!invariant_result.config->core.disabled,
				             "consumed object is original disabled state");
			}
		}
		fs::remove(invariant_path, ec);
		fs::remove(backup_invariant, ec);

		ok &= Expect(WriteFile(invariant_path, "[core]\ndisabled = false\n"),
		             "write insecure opened config");
		ok &= Expect(chmod(invariant_path.c_str(), 0666) == 0, "make initially world-writable");

		bool insecure_replacement_ok = false;
		{
			const howdy::native::config_test_hooks::ScopedHooks hooks([&]() -> void {
				insecure_replacement_ok =
				    rename(invariant_path.c_str(), backup_invariant.c_str()) == 0 &&
				    WriteFile(invariant_path, "[core]\ndisabled = false\n") &&
				    chmod(invariant_path.c_str(), 0644) == 0;
			});

			const auto insecure_opened_result =
			    howdy::native::LoadRuntimeConfig(invariant_path, std::nullopt, {temp_root});
			ok &= Expect(insecure_replacement_ok,
			             "insecure invariant pathname replacement succeeded");
			ok &= ExpectFailure(insecure_opened_result, RuntimeConfigLoadStatus::kPathError,
			                    "opened descriptor with insecure mode is rejected even if "
			                    "disk path replaced with safe file");
		}
	}

	const auto missing_path = temp_root / "missing.ini";
	const auto missing = howdy::native::LoadRuntimeConfig(missing_path, std::nullopt, {temp_root});
	ok &= ExpectFailure(missing, RuntimeConfigLoadStatus::kPathError, "missing config");
	ok &= Expect(missing.error_code == ENOENT, "missing config preserves open errno");

	const auto malformed_path = temp_root / "malformed.ini";
	ok &= Expect(WriteFile(malformed_path, "[core\n"), "write malformed config");
	const auto malformed =
	    howdy::native::LoadRuntimeConfig(malformed_path, std::nullopt, {temp_root});
	ok &= ExpectFailure(malformed, RuntimeConfigLoadStatus::kParseError, "malformed config");
	ok &= Expect(malformed.error_message.contains("Failed to parse config: "),
	             "parse error message includes parse prefix");
	ok &= Expect(malformed.error_message.contains(malformed_path.string()),
	             "parse error message includes path");
	ok &= Expect(malformed.error_message.contains("(error "),
	             "parse error message includes parser code");

	const auto invalid_path = temp_root / "invalid.ini";
	ok &= Expect(WriteFile(invalid_path, "[video]\ntimeout = 0\n"), "write invalid config");
	const auto invalid = howdy::native::LoadRuntimeConfig(invalid_path, std::nullopt, {temp_root});
	ok &= ExpectFailure(invalid, RuntimeConfigLoadStatus::kInvalidRuntimeValue,
	                    "invalid runtime config");
	ok &= Expect(invalid.error_message.contains("Invalid runtime config in "),
	             "invalid runtime message includes prefix");
	ok &= Expect(invalid.error_message.contains(invalid_path.string()),
	             "invalid runtime message includes path");
	ok &= Expect(invalid.error_message.contains(invalid_path.string() + ": "),
	             "invalid runtime message separates path and validation detail");

	const auto insecure_root = temp_root / "insecure-root";
	fs::create_directories(insecure_root, ec);
	ok &= Expect(!ec, "create insecure trust root");
	const auto insecure_root_config = insecure_root / "config.ini";
	ok &= Expect(WriteFile(insecure_root_config, "[core]\ndisabled = false\n"),
	             "write insecure trust-root config");
	ok &= Expect(chmod(insecure_root.c_str(), 0777) == 0, "make trust root insecure");
	const auto insecure_trust =
	    howdy::native::LoadRuntimeConfig(insecure_root_config, std::nullopt, {temp_root});
	ok &= ExpectFailure(insecure_trust, RuntimeConfigLoadStatus::kPathError, "insecure trust root");

	if (!ok) {
		return 1;
	}
	return 0;
}
