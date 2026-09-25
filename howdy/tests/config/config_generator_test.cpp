#include "test_support.hpp"

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/resource.h>
#include <sys/wait.h>

namespace {

	using howdy::test::CountFilesWithPrefix;
	using howdy::test::Expect;
	using howdy::test::ReadFile;
	using howdy::test::WriteFile;
	namespace fs = std::filesystem;

	auto RunGenerator(const fs::path &generator_path, const std::vector<std::string> &emulator,
	                  std::vector<std::string> arguments, bool limit_file_size) -> int {
		arguments.insert(arguments.begin(), generator_path.string());
		arguments.insert(arguments.begin(), emulator.begin(), emulator.end());
		const pid_t child = fork();
		if (child < 0) {
			return -1;
		}
		if (child == 0) {
			if (limit_file_size) {
				if (std::signal(SIGXFSZ, SIG_IGN) == SIG_ERR) {
					_exit(125);
				}
				const rlimit limit{.rlim_cur = 0, .rlim_max = 0};
				if (setrlimit(RLIMIT_FSIZE, &limit) != 0) {
					_exit(125);
				}
			}

			std::vector<char *> argv;
			argv.reserve(arguments.size() + 1);
			for (auto &argument : arguments) {
				argv.push_back(argument.data());
			}
			argv.push_back(nullptr);
			execvp(arguments.front().c_str(), argv.data());
			_exit(127);
		}

		int status = 0;
		while (waitpid(child, &status, 0) < 0) {
			if (errno != EINTR) {
				return -1;
			}
		}
		if (WIFEXITED(status)) {
			return WEXITSTATUS(status);
		}
		if (WIFSIGNALED(status)) {
			return 128 + WTERMSIG(status);
		}
		return -1;
	}

}  // namespace

auto main(int argc, char **argv) -> int {
	const auto arguments =
	    std::span(argv, argc > 0 ? static_cast<std::size_t>(argc) : std::size_t{0});
	if (arguments.size() < 2) {
		return 1;
	}
	const fs::path                 generator_path(arguments.subspan(1, 1).front());
	const std::vector<std::string> emulator(arguments.begin() + 2, arguments.end());
	bool                           ok = true;

	const auto root =
	    fs::temp_directory_path() / ("howdy-config-generator-test-" + std::to_string(getpid()));
	std::error_code error;
	fs::remove_all(root, error);
	fs::create_directories(root, error);
	ok &= Expect(!error, "create generator test directory");
	if (error) {
		return 1;
	}

	ok &= Expect(RunGenerator(generator_path, emulator, {}, false) == 2,
	             "missing generator arguments return usage status");
	ok &= Expect(RunGenerator(generator_path, emulator, {"--output"}, false) == 2,
	             "incomplete output argument returns usage status");

	const auto output = root / "generated" / "config.ini";
	error.clear();
	fs::create_directories(output.parent_path(), error);
	ok &= Expect(!error, "create generator output directory");
	const auto unrelated_temporary = fs::path(output.string() + ".tmp");
	ok &= Expect(WriteFile(unrelated_temporary, "must remain untouched\n"),
	             "create unrelated output.tmp file");
	ok &= Expect(RunGenerator(generator_path, emulator, {"--output", output.string()}, false) == 0,
	             "generator writes new output");
	const auto generated_content = ReadFile(output);
	ok &= Expect(!generated_content.empty(), "successful generation creates non-empty output");
	ok &= Expect(ReadFile(unrelated_temporary) == "must remain untouched\n",
	             "generator preserves unrelated output.tmp file");

	ok &= Expect(WriteFile(output, "old config\n"), "create existing output");
	ok &= Expect(RunGenerator(generator_path, emulator, {"--output", output.string()}, false) == 0,
	             "generator replaces existing output");
	ok &= Expect(ReadFile(output) == generated_content,
	             "replacement output is deterministic generated content");

	const auto failed_output = root / "failed" / "config.ini";
	fs::create_directories(failed_output.parent_path(), error);
	ok &= Expect(!error && WriteFile(failed_output, "preserve this config\n"),
	             "create output for failed-write test");
	const auto staged_before = CountFilesWithPrefix(failed_output.parent_path(), ".howdy-atomic-");
	const int  failed_write_status =
	    RunGenerator(generator_path, emulator, {"--output", failed_output.string()}, true);
	ok &= Expect(failed_write_status != 0, "failed atomic write returns nonzero");
	ok &= Expect(ReadFile(failed_output) == "preserve this config\n",
	             "failed atomic write preserves existing output");
	ok &=
	    Expect(CountFilesWithPrefix(failed_output.parent_path(), ".howdy-atomic-") == staged_before,
	           "failed atomic write cleans staged files");

	const auto blocked_parent = root / "blocked-parent";
	ok &= Expect(WriteFile(blocked_parent, "not a directory\n"), "create invalid output parent");
	ok &= Expect(RunGenerator(generator_path, emulator,
	                          {"--output", (blocked_parent / "config.ini").string()}, false) != 0,
	             "invalid output directory returns nonzero");
	ok &= Expect(ReadFile(blocked_parent) == "not a directory\n",
	             "invalid output directory remains unchanged");

	fs::remove_all(root, error);
	ok &= Expect(!error, "remove generator test directory");
	return ok ? 0 : 1;
}
