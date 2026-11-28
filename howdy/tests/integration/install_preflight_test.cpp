#include "support/eintr.hpp"
#include "test_support.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/wait.h>

namespace {

	namespace fs = std::filesystem;
	using howdy::test::Expect;
	using howdy::test::WriteFile;

	struct CommandOutput {
		bool        exited = false;
		int         status = -1;
		std::string stderr_text;
	};

	class PreflightRunner {
	public:
		explicit PreflightRunner(std::vector<std::string> base_command)
		    : base_command_(std::move(base_command)) {}

		[[nodiscard]] auto Run(const std::vector<std::string> &args) const -> CommandOutput {
			CommandOutput      output;
			std::array<int, 2> err_pipe{-1, -1};
			if (pipe2(err_pipe.data(), O_CLOEXEC) != 0) {
				return output;
			}
			const howdy::test::ScopedFd err_read(err_pipe.at(0));
			howdy::test::ScopedFd       err_write(err_pipe.at(1));

			posix_spawn_file_actions_t actions;
			if (posix_spawn_file_actions_init(&actions) != 0) {
				return output;
			}
			const bool actions_ok =
			    posix_spawn_file_actions_adddup2(&actions, err_write.Get(), STDERR_FILENO) == 0 &&
			    posix_spawn_file_actions_addclose(&actions, err_read.Get()) == 0 &&
			    posix_spawn_file_actions_addclose(&actions, err_write.Get()) == 0;
			if (!actions_ok) {
				(void)posix_spawn_file_actions_destroy(&actions);
				return output;
			}

			std::vector<std::string> storage = base_command_;
			storage.insert(storage.end(), args.begin(), args.end());
			std::vector<char *> argv;
			argv.reserve(storage.size() + 1);
			for (auto &arg : storage) {
				argv.push_back(arg.data());
			}
			argv.push_back(nullptr);

			pid_t     pid = -1;
			const int spawn_result =
			    posix_spawnp(&pid, argv.front(), &actions, nullptr, argv.data(), environ);
			(void)posix_spawn_file_actions_destroy(&actions);
			if (spawn_result != 0) {
				return output;
			}
			// Close our write end so read() hits EOF once the child exits.
			err_write.Reset();

			std::array<char, 256> buffer{};
			while (true) {
				const ssize_t bytes = howdy::native::RetryOnEintr([&] -> ssize_t {
					return read(err_read.Get(), buffer.data(), buffer.size());
				});
				if (bytes <= 0) {
					break;
				}
				output.stderr_text.append(buffer.data(), static_cast<std::size_t>(bytes));
			}

			int         wait_status = 0;
			const pid_t waited      = howdy::native::RetryOnEintr([&] -> pid_t {
				return waitpid(pid, &wait_status, 0);
			});
			if (waited == pid && WIFEXITED(wait_status)) {
				output.exited = true;
				output.status = WEXITSTATUS(wait_status);
			}
			return output;
		}

	private:
		std::vector<std::string> base_command_;
	};

}  // namespace

auto main(int argc, char **argv) -> int {
	const auto raw_args =
	    std::span(argv, argc > 0 ? static_cast<std::size_t>(argc) : std::size_t{0});
	if (raw_args.size() < 3) {
		std::cerr << "Usage: install_preflight_test <preflight-binary> <work-dir> [emulator...]\n";
		return 1;
	}

	std::vector<std::string> base_command;
	for (const char *part : raw_args.subspan(3)) {
		base_command.emplace_back(part);
	}
	base_command.emplace_back(raw_args.subspan(1).front());
	const PreflightRunner runner(std::move(base_command));

	const fs::path  raw_work_root(raw_args.subspan(2).front());
	std::error_code ec;
	fs::remove_all(raw_work_root, ec);
	ec.clear();
	fs::create_directories(raw_work_root, ec);

	bool ok = Expect(!ec, "create install_preflight test root");
	if (!ok) {
		return 1;
	}
	// Canonicalize the workspace root so symlinked build directories or relative <work-dir>
	// arguments do not trip install_preflight's ancestor-symlink checks before reaching test cases.
	const fs::path work_root = fs::canonical(raw_work_root, ec);
	ok &= Expect(!ec, "canonicalize install_preflight test root");
	if (!ok) {
		return 1;
	}

	const fs::path stage = work_root / "stage";
	fs::create_directories(stage / "usr" / "bin", ec);
	ok &= Expect(!ec, "create clean staging directory");
	const fs::path existing_file = stage / "usr" / "bin" / "existing";
	ok &= Expect(WriteFile(existing_file, "ok\n"), "write existing clean file");

	// 1. Valid manifest with existing directory, existing single-link file, and new paths passes.
	const fs::path valid_manifest = work_root / "valid.manifest";
	ok &= Expect(
	    WriteFile(valid_manifest,
		          "D/usr/bin\nF/usr/bin/existing\nD/usr/share/howdy\nF/usr/share/howdy/new.dat\n"),
	    "write valid manifest");
	{
		const auto result = runner.Run({stage.string(), valid_manifest.string()});
		ok &= Expect(result.exited && result.status == 0 && result.stderr_text.empty(),
		             "valid manifest succeeds: " + result.stderr_text);
	}

	// 2. Argument count and DESTDIR validation failures.
	struct ArgFailureCase {
		const char              *label;
		std::vector<std::string> args;
		std::string              expected_error;
	};

	const fs::path symlink_stage = work_root / "symlink-stage";
	ok &= Expect(symlink(stage.c_str(), symlink_stage.c_str()) == 0, "create symlink DESTDIR");
	const fs::path file_stage = work_root / "file-stage";
	ok &= Expect(WriteFile(file_stage, "not-a-dir"), "create file DESTDIR");

	const auto arg_cases = std::to_array<ArgFailureCase>({
	    {
	        .label          = "missing arguments",
	        .args           = {},
	        .expected_error = "usage: install_preflight DESTDIR MANIFEST",
	    },
	    {
	        .label          = "single argument",
	        .args           = {stage.string()},
	        .expected_error = "usage: install_preflight DESTDIR MANIFEST",
	    },
	    {
	        .label          = "extra argument",
	        .args           = {stage.string(), valid_manifest.string(), "extra"},
	        .expected_error = "usage: install_preflight DESTDIR MANIFEST",
	    },
	    {
	        .label          = "relative DESTDIR",
	        .args           = {"relative/stage", valid_manifest.string()},
	        .expected_error = "DESTDIR must be absolute",
	    },
	    {
	        .label          = "DESTDIR with parent traversal",
	        .args           = {(stage / ".." / "stage").string(), valid_manifest.string()},
	        .expected_error = "DESTDIR must not contain '..'",
	    },
	    {
	        .label          = "symlink DESTDIR",
	        .args           = {symlink_stage.string(), valid_manifest.string()},
	        .expected_error = "Unsafe install destination: " + symlink_stage.string(),
	    },
	    {
	        .label          = "regular-file DESTDIR",
	        .args           = {file_stage.string(), valid_manifest.string()},
	        .expected_error = "Unsafe install destination: " + file_stage.string(),
	    },
	    {
	        .label          = "missing manifest file",
	        .args           = {stage.string(), (work_root / "missing.manifest").string()},
	        .expected_error = "Cannot read install destination manifest",
	    },
	});
	for (const auto &test_case : arg_cases) {
		const auto result = runner.Run(test_case.args);
		ok &= Expect(result.exited && result.status == 1 &&
		                 result.stderr_text.contains(test_case.expected_error),
		             std::string("rejects ") + test_case.label + ": " + result.stderr_text);
	}

	// 3. Malformed manifest lines and path escapes.
	struct ManifestFailureCase {
		const char      *label;
		const char      *content;
		std::string_view expected_error;
	};

	const auto manifest_cases = std::to_array<ManifestFailureCase>({
	    {
	        .label          = "empty manifest line",
	        .content        = "\n",
	        .expected_error = "Invalid install destination manifest",
	    },
	    {
	        .label          = "single-character manifest line",
	        .content        = "F\n",
	        .expected_error = "Invalid install destination manifest",
	    },
	    {
	        .label          = "unknown manifest record type",
	        .content        = "X/usr/bin/howdy\n",
	        .expected_error = "Invalid install destination manifest",
	    },
	    {
	        .label          = "relative file destination",
	        .content        = "Fusr/bin/howdy\n",
	        .expected_error = "Install destination escapes DESTDIR",
	    },
	    {
	        .label          = "relative directory destination",
	        .content        = "Dusr/bin\n",
	        .expected_error = "Install destination escapes DESTDIR",
	    },
	    {
	        .label          = "file destination escaping DESTDIR",
	        .content        = "F/../../etc/passwd\n",
	        .expected_error = "Install destination escapes DESTDIR",
	    },
	    {
	        .label          = "directory destination escaping DESTDIR",
	        .content        = "D/../../etc\n",
	        .expected_error = "Install destination escapes DESTDIR",
	    },
	    {
	        .label          = "file destination resolving to DESTDIR root",
	        .content        = "F/\n",
	        .expected_error = "Install destination escapes DESTDIR",
	    },
	});
	for (const auto &test_case : manifest_cases) {
		const fs::path bad_manifest = work_root / "bad.manifest";
		ok &= Expect(WriteFile(bad_manifest, test_case.content),
		             std::string("write manifest for ") + test_case.label);
		const auto result = runner.Run({stage.string(), bad_manifest.string()});
		ok &= Expect(result.exited && result.status == 1 &&
		                 result.stderr_text.contains(test_case.expected_error),
		             std::string("rejects ") + test_case.label + ": " + result.stderr_text);
	}

	// 4. Unsafe existing destinations inside DESTDIR.
	const fs::path unsafe_stage = work_root / "unsafe-stage";
	fs::create_directories(unsafe_stage / "usr" / "bin", ec);
	ok &= Expect(!ec, "create unsafe-stage tree");
	const fs::path outside_target = work_root / "outside-file";
	ok &= Expect(WriteFile(outside_target, "victim"), "write outside target");
	const fs::path symlink_leaf = unsafe_stage / "usr" / "bin" / "link-leaf";
	ok &= Expect(symlink(outside_target.c_str(), symlink_leaf.c_str()) == 0, "create symlink leaf");
	const fs::path symlink_dir = unsafe_stage / "usr" / "symdir";
	ok &= Expect(symlink(work_root.c_str(), symlink_dir.c_str()) == 0, "create symlink dir");
	const fs::path hardlink_leaf = unsafe_stage / "usr" / "bin" / "hardlink-leaf";
	ok &= Expect(link(outside_target.c_str(), hardlink_leaf.c_str()) == 0, "create hardlink leaf");
	const fs::path regular_for_dir = unsafe_stage / "usr" / "file-not-dir";
	ok &= Expect(WriteFile(regular_for_dir, "data"), "write regular file for dir conflict");

	struct UnsafeDestCase {
		const char *label;
		const char *content;
		std::string expected_error;
	};

	const auto unsafe_dest_cases = std::to_array<UnsafeDestCase>({
	    {
	        .label          = "symlink file leaf",
	        .content        = "F/usr/bin/link-leaf\n",
	        .expected_error = "Unsafe install destination: " + symlink_leaf.string(),
	    },
	    {
	        .label          = "hard-linked file leaf",
	        .content        = "F/usr/bin/hardlink-leaf\n",
	        .expected_error = "Unsafe install destination: " + hardlink_leaf.string(),
	    },
	    {
	        .label   = "existing directory at file destination",
	        .content = "F/usr/bin\n",
	        .expected_error =
	            "Unsafe install destination: " + (unsafe_stage / "usr" / "bin").string(),
	    },
	    {
	        .label          = "existing regular file at directory destination",
	        .content        = "D/usr/file-not-dir\n",
	        .expected_error = "Unsafe install destination: " + regular_for_dir.string(),
	    },
	    {
	        .label          = "symlink ancestor directory",
	        .content        = "F/usr/symdir/child\n",
	        .expected_error = "Unsafe install destination: " + symlink_dir.string(),
	    },
	});
	for (const auto &test_case : unsafe_dest_cases) {
		const fs::path unsafe_manifest = work_root / "unsafe.manifest";
		ok &= Expect(WriteFile(unsafe_manifest, test_case.content),
		             std::string("write unsafe manifest for ") + test_case.label);
		const auto result = runner.Run({unsafe_stage.string(), unsafe_manifest.string()});
		ok &= Expect(result.exited && result.status == 1 &&
		                 result.stderr_text.contains(test_case.expected_error),
		             std::string("rejects ") + test_case.label + ": " + result.stderr_text);
	}

	fs::remove_all(work_root, ec);
	return ok ? 0 : 1;
}
