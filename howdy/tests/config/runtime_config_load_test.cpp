#include "config/config_limits.hpp"
#include "config/runtime_config_loader.hpp"
#include "config/runtime_paths.hpp"
#include "config/test_hooks.hpp"
#include "support/file_security.hpp"
#include "test_support.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>

namespace {

	using howdy::test::CreateTempDirectory;
	using howdy::test::Expect;
	using howdy::test::TemporaryDirectory;
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

	auto WriteSecureConfig(const std::filesystem::path &path, const std::string &content) -> bool {
		return WriteFile(path, content) && chmod(path.c_str(), 0644) == 0;
	}

	// Descriptors stand in for ones another process opened: no path is walked, so only the
	// descriptor itself decides, and the shared file offset must not move.
	auto ExpectDescriptorLoading(const std::filesystem::path &temp_root) -> bool {
		using howdy::native::LoadRuntimeConfigFromFd;
		using howdy::native::RuntimeConfigLoadStatus;
		using howdy::test::ScopedFd;
		bool                        ok           = true;
		const uid_t                 owner        = geteuid();
		const std::filesystem::path display_path = "/etc/howdy/config.ini";

		const auto valid_path = temp_root / "fd-valid.ini";
		ok &=
		    Expect(WriteSecureConfig(valid_path, "[core]\ndisabled = true\n[video]\ntimeout = 7\n"),
			       "write descriptor config");
		const ScopedFd valid_fd(open(valid_path.c_str(), O_RDONLY | O_CLOEXEC));
		ok &= Expect(valid_fd.Valid() && lseek(valid_fd.Get(), 3, SEEK_SET) == 3,
		             "open descriptor config away from offset 0");
		const auto valid = LoadRuntimeConfigFromFd(valid_fd.Get(), display_path, owner);
		ok &= Expect(valid.ok && valid.status == RuntimeConfigLoadStatus::kOk &&
		                 valid.config.has_value() && valid.config->core.disabled &&
		                 valid.config->video.timeout == 7,
		             "descriptor config loads from the start of the file");
		ok &= Expect(valid.path == display_path && valid.error_message.empty(),
		             "descriptor config reports the display path");
		ok &= Expect(lseek(valid_fd.Get(), 0, SEEK_CUR) == 3,
		             "descriptor config load leaves the shared offset alone");
		ok &= Expect(LoadRuntimeConfigFromFd(valid_fd.Get(), display_path, owner).ok,
		             "descriptor config loads again from the same descriptor");

		{
			// The descriptor keeps naming the inode it was opened on and never reads the path.
			const auto snapshot_path = temp_root / "fd-snapshot.ini";
			const auto moved_path    = temp_root / "fd-snapshot-moved.ini";
			ok &= Expect(WriteSecureConfig(snapshot_path, "[video]\ntimeout = 7\n"),
			             "write snapshot descriptor config");
			const ScopedFd snapshot_fd(open(snapshot_path.c_str(), O_RDONLY | O_CLOEXEC));
			ok &= Expect(rename(snapshot_path.c_str(), moved_path.c_str()) == 0 &&
			                 WriteSecureConfig(snapshot_path, "[video]\ntimeout = 9\n"),
			             "move snapshot config and write another at its path");
			const auto moved = LoadRuntimeConfigFromFd(snapshot_fd.Get(), display_path, owner);
			ok &= Expect(moved.ok && moved.config.has_value() && moved.config->video.timeout == 7,
			             "descriptor config ignores what its old path now names");

			// Once the file is unlinked (replaced or removed) it has no link left, and the
			// load fails closed instead of using the stale contents.
			ok &= Expect(unlink(moved_path.c_str()) == 0, "unlink snapshot descriptor config");
			ok &= ExpectFailure(LoadRuntimeConfigFromFd(snapshot_fd.Get(), display_path, owner),
			                    RuntimeConfigLoadStatus::kPathError, "unlinked descriptor config");
		}

		const auto wrong_owner =
		    LoadRuntimeConfigFromFd(valid_fd.Get(), display_path, static_cast<uid_t>(owner + 1));
		ok &= ExpectFailure(wrong_owner, RuntimeConfigLoadStatus::kPathError,
		                    "descriptor config with another owner");
		ok &= Expect(wrong_owner.error_message.contains("owned by UID") &&
		                 wrong_owner.error_message.contains(display_path.string()),
		             "descriptor owner error names the display path");

		ok &= Expect(fchmod(valid_fd.Get(), 0664) == 0, "make descriptor config group-writable");
		ok &=
		    ExpectFailure(LoadRuntimeConfigFromFd(valid_fd.Get(), display_path, owner),
			              RuntimeConfigLoadStatus::kPathError, "group-writable descriptor config");
		ok &= Expect(fchmod(valid_fd.Get(), 0646) == 0, "make descriptor config world-writable");
		ok &=
		    ExpectFailure(LoadRuntimeConfigFromFd(valid_fd.Get(), display_path, owner),
			              RuntimeConfigLoadStatus::kPathError, "world-writable descriptor config");
		ok &= Expect(fchmod(valid_fd.Get(), 0644) == 0, "restore descriptor config mode");

		const auto linked_path = temp_root / "fd-linked.ini";
		ok &= Expect(link(valid_path.c_str(), linked_path.c_str()) == 0,
		             "hard-link descriptor config");
		const ScopedFd linked_fd(open(valid_path.c_str(), O_RDONLY | O_CLOEXEC));
		const auto     linked = LoadRuntimeConfigFromFd(linked_fd.Get(), display_path, owner);
		ok &= ExpectFailure(linked, RuntimeConfigLoadStatus::kPathError,
		                    "hard-linked descriptor config");
		ok &= Expect(linked.error_message.contains("hard-linked"),
		             "hard-linked descriptor config reports the link");
		ok &= Expect(unlink(linked_path.c_str()) == 0, "remove descriptor config hard link");

		const ScopedFd read_write_fd(open(valid_path.c_str(), O_RDWR | O_CLOEXEC));
		const auto read_write = LoadRuntimeConfigFromFd(read_write_fd.Get(), display_path, owner);
		ok &= ExpectFailure(read_write, RuntimeConfigLoadStatus::kPathError,
		                    "writable descriptor config");
		ok &= Expect(read_write.error_message.contains("must be open read-only"),
		             "writable descriptor config reports its open mode");

		const ScopedFd path_fd(open(valid_path.c_str(), O_PATH | O_CLOEXEC));
		ok &= ExpectFailure(LoadRuntimeConfigFromFd(path_fd.Get(), display_path, owner),
		                    RuntimeConfigLoadStatus::kPathError, "O_PATH descriptor config");

		const ScopedFd directory_fd(open(temp_root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
		ok &= ExpectFailure(LoadRuntimeConfigFromFd(directory_fd.Get(), display_path, owner),
		                    RuntimeConfigLoadStatus::kPathError, "directory descriptor config");

		std::array<int, 2> pipe_fds{{-1, -1}};
		ok &= Expect(pipe2(pipe_fds.data(), O_CLOEXEC) == 0, "create descriptor config pipe");
		const ScopedFd pipe_read(pipe_fds.at(0));
		const ScopedFd pipe_write(pipe_fds.at(1));
		ok &= ExpectFailure(LoadRuntimeConfigFromFd(pipe_read.Get(), display_path, owner),
		                    RuntimeConfigLoadStatus::kPathError, "pipe descriptor config");

		const auto invalid_fd = LoadRuntimeConfigFromFd(-1, display_path, owner);
		ok &= ExpectFailure(invalid_fd, RuntimeConfigLoadStatus::kPathError,
		                    "negative descriptor config");
		ok &= Expect(invalid_fd.error_code == EBADF, "negative descriptor config is EBADF");

		const auto large_path = temp_root / "fd-large.ini";
		ok &= Expect(
		    WriteSecureConfig(large_path, "[core]\ndisabled = false\n" +
			                                  std::string(howdy::native::kMaxConfigFileSize, '\n')),
		    "write oversized descriptor config");
		const ScopedFd large_fd(open(large_path.c_str(), O_RDONLY | O_CLOEXEC));
		ok &= ExpectFailure(LoadRuntimeConfigFromFd(large_fd.Get(), display_path, owner),
		                    RuntimeConfigLoadStatus::kParseError, "oversized descriptor config");

		const auto malformed_path = temp_root / "fd-malformed.ini";
		ok &= Expect(WriteSecureConfig(malformed_path, "[core\n"),
		             "write malformed descriptor config");
		const ScopedFd malformed_fd(open(malformed_path.c_str(), O_RDONLY | O_CLOEXEC));
		const auto     malformed = LoadRuntimeConfigFromFd(malformed_fd.Get(), display_path, owner);
		ok &= ExpectFailure(malformed, RuntimeConfigLoadStatus::kParseError,
		                    "malformed descriptor config");
		ok &= Expect(malformed.error_message.contains(display_path.string()),
		             "malformed descriptor config names the display path");

		const auto zeroed_path = temp_root / "fd-zeroed.ini";
		ok &= Expect(WriteSecureConfig(zeroed_path, std::string(64, '\0')),
		             "write zero-filled descriptor config");
		const ScopedFd zeroed_fd(open(zeroed_path.c_str(), O_RDONLY | O_CLOEXEC));
		const auto     zeroed = LoadRuntimeConfigFromFd(zeroed_fd.Get(), display_path, owner);
		ok &= ExpectFailure(zeroed, RuntimeConfigLoadStatus::kParseError,
		                    "zero-filled descriptor config");
		ok &= Expect(zeroed.error_message ==
		                 "Config file contains a NUL byte: " + display_path.string() +
		                     "; repair it with \"howdy config\"",
		             "zero-filled descriptor config says why: " + zeroed.error_message);

		const auto invalid_value_path = temp_root / "fd-invalid.ini";
		ok &= Expect(WriteSecureConfig(invalid_value_path, "[video]\ntimeout = 0\n"),
		             "write invalid descriptor config");
		const ScopedFd invalid_value_fd(open(invalid_value_path.c_str(), O_RDONLY | O_CLOEXEC));
		ok &= ExpectFailure(LoadRuntimeConfigFromFd(invalid_value_fd.Get(), display_path, owner),
		                    RuntimeConfigLoadStatus::kInvalidRuntimeValue,
		                    "invalid descriptor config value");
		return ok;
	}

}  // namespace

auto main() -> int {
	namespace fs = std::filesystem;
	using howdy::native::RuntimeConfigLoadStatus;

	bool ok = true;

	const auto temp_directory = CreateTempDirectory("howdy-runtime-config-load-test-");
	if (!temp_directory.has_value()) {
		std::cerr << "FAIL: create unique temp root\n";
		return 1;
	}
	const TemporaryDirectory temp_directory_guard(*temp_directory);
	const auto              &temp_root = temp_directory_guard.Path();
	ok &= Expect(chmod(temp_root.c_str(), 0755) == 0, "secure temp root");

	const auto valid_path = temp_root / "valid.ini";
	ok &= Expect(WriteFile(valid_path, "[core]\ndisabled = false\n"), "write valid config");
	ok &= Expect(chmod(valid_path.c_str(), 0644) == 0, "secure valid config");
	setenv("HOWDY_CONFIG", valid_path.c_str(), 1);
	auto const valid = howdy::native::LoadRuntimeConfig(
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

	ok &= ExpectFailure(howdy::native::LoadRuntimeConfig(
	                        valid_path, static_cast<uid_t>(geteuid() + 1), {temp_root}),
	                    RuntimeConfigLoadStatus::kPathError, "mismatched owner_uid is rejected");
	ok &= Expect(
	    howdy::native::LoadRuntimeConfig(valid_path, static_cast<uid_t>(geteuid() + 1), {temp_root})
	        .error_message.contains("owned by UID"),
	    "mismatched owner_uid reports ownership error");

	const auto      symlink_path = temp_root / "link.ini";
	std::error_code link_ec;
	fs::create_symlink(valid_path, symlink_path, link_ec);
	ok &= Expect(!link_ec, "create symlinked config");
	const auto symlink_result =
	    howdy::native::LoadRuntimeConfig(symlink_path, std::nullopt, {temp_root});
	ok &= ExpectFailure(symlink_result, RuntimeConfigLoadStatus::kPathError,
	                    "symlinked config is rejected");
	ok &= Expect(symlink_result.error_code == ELOOP, "symlinked config preserves ELOOP");

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
	ok &= ExpectFailure(large_runtime, RuntimeConfigLoadStatus::kParseError,
	                    "runtime config rejects content above bounded config size");

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
			const howdy::native::config_test_hooks::ScopedHooks hooks([&] -> void {
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
			const howdy::native::config_test_hooks::ScopedHooks hooks([&] -> void {
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

	for (const bool replacement_secure : {true, false}) {
		// Another writer renames a config over the one just opened.
		const auto replaced_path = temp_root / "replaced.ini";
		const auto next_path     = temp_root / "replaced-next.ini";
		ok &= Expect(WriteSecureConfig(replaced_path, "[video]\ntimeout = 7\n") &&
		                 WriteSecureConfig(next_path, "[video]\ntimeout = 9\n") &&
		                 chmod(next_path.c_str(), replacement_secure ? 0644 : 0666) == 0,
		             "write config to be replaced while it is read");
		const howdy::native::config_test_hooks::ScopedHooks hooks([&] -> void {
			(void)rename(next_path.c_str(), replaced_path.c_str());
		});

		const auto replaced =
		    howdy::native::LoadRuntimeConfig(replaced_path, std::nullopt, {temp_root});
		if (replacement_secure) {
			ok &= Expect(replaced.config.has_value() && replaced.config->video.timeout == 9,
			             "config replaced during the load is loaded: " + replaced.error_message);
		} else {
			ok &= ExpectFailure(replaced, RuntimeConfigLoadStatus::kPathError,
			                    "insecure replacement during the load");
		}
	}

	const auto missing_path = temp_root / "missing.ini";
	const auto missing = howdy::native::LoadRuntimeConfig(missing_path, std::nullopt, {temp_root});
	ok &= ExpectFailure(missing, RuntimeConfigLoadStatus::kPathError, "missing config");
	ok &= Expect(missing.error_code == ENOENT, "missing config preserves open errno");

	const auto empty_path = temp_root / "empty.ini";
	ok &= Expect(WriteSecureConfig(empty_path, ""), "write empty config");
	const auto empty = howdy::native::LoadRuntimeConfig(empty_path, std::nullopt, {temp_root});
	ok &= ExpectFailure(empty, RuntimeConfigLoadStatus::kParseError, "empty config");
	ok &= Expect(empty.error_message == "Config file is blank: " + empty_path.string() +
	                                        "; repair it with \"howdy config\"",
	             "empty config says why: " + empty.error_message);
	ok &= Expect(WriteSecureConfig(empty_path, "# defaults\n") &&
	                 howdy::native::LoadRuntimeConfig(empty_path, std::nullopt, {temp_root}).ok,
	             "comments-only config loads");

	const auto malformed_path = temp_root / "malformed.ini";
	ok &= Expect(WriteFile(malformed_path, "[core\n"), "write malformed config");
	ok &= Expect(chmod(malformed_path.c_str(), 0644) == 0, "secure malformed config");
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
	ok &= Expect(chmod(invalid_path.c_str(), 0644) == 0, "secure invalid config");
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

	ok &= ExpectDescriptorLoading(temp_root);

	return ok ? 0 : 1;
}
