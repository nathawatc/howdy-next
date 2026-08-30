#include "test_support.hpp"

#include <cerrno>
#include <csignal>
#include <filesystem>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include <sys/resource.h>
#include <sys/wait.h>

namespace {

	using howdy::test::count_files_with_prefix;
	using howdy::test::expect;
	using howdy::test::read_file;
	using howdy::test::write_file;
	namespace fs = std::filesystem;

	auto run_generator(const fs::path &generator_path, std::vector<std::string> arguments,
	                   bool limit_file_size) -> int {
		arguments.insert(arguments.begin(), generator_path.string());
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
			execv(arguments.front().c_str(), argv.data());
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
	if (argc != 2) {
		return 1;
	}
	const fs::path generator_path(argv[1]);
	bool           ok = true;

	const auto root =
	    fs::temp_directory_path() / ("howdy-config-generator-test-" + std::to_string(getpid()));
	std::error_code error;
	fs::remove_all(root, error);
	fs::create_directories(root, error);
	ok &= expect(!error, "create generator test directory");
	if (error) {
		return 1;
	}

	ok &= expect(run_generator(generator_path, {}, false) == 2,
	             "missing generator arguments return usage status");
	ok &= expect(run_generator(generator_path, {"--output"}, false) == 2,
	             "incomplete output argument returns usage status");

	const auto output = root / "generated" / "config.ini";
	error.clear();
	fs::create_directories(output.parent_path(), error);
	ok &= expect(!error, "create generator output directory");
	const auto unrelated_temporary = fs::path(output.string() + ".tmp");
	ok &= expect(write_file(unrelated_temporary, "must remain untouched\n"),
	             "create unrelated output.tmp file");
	ok &= expect(run_generator(generator_path, {"--output", output.string()}, false) == 0,
	             "generator writes new output");
	const auto generated_content = read_file(output);
	ok &= expect(!generated_content.empty(), "successful generation creates non-empty output");
	ok &= expect(read_file(unrelated_temporary) == "must remain untouched\n",
	             "generator preserves unrelated output.tmp file");

	ok &= expect(write_file(output, "old config\n"), "create existing output");
	ok &= expect(run_generator(generator_path, {"--output", output.string()}, false) == 0,
	             "generator replaces existing output");
	ok &= expect(read_file(output) == generated_content,
	             "replacement output is deterministic generated content");

	const auto failed_output = root / "failed" / "config.ini";
	fs::create_directories(failed_output.parent_path(), error);
	ok &= expect(!error && write_file(failed_output, "preserve this config\n"),
	             "create output for failed-write test");
	const auto staged_before =
	    count_files_with_prefix(failed_output.parent_path(), ".howdy-atomic-");
	const int failed_write_status =
	    run_generator(generator_path, {"--output", failed_output.string()}, true);
	ok &= expect(failed_write_status != 0, "failed atomic write returns nonzero");
	ok &= expect(read_file(failed_output) == "preserve this config\n",
	             "failed atomic write preserves existing output");
	ok &= expect(count_files_with_prefix(failed_output.parent_path(), ".howdy-atomic-") ==
	                 staged_before,
	             "failed atomic write cleans staged files");

	const auto blocked_parent = root / "blocked-parent";
	ok &= expect(write_file(blocked_parent, "not a directory\n"), "create invalid output parent");
	ok &= expect(run_generator(generator_path,
	                           {"--output", (blocked_parent / "config.ini").string()}, false) != 0,
	             "invalid output directory returns nonzero");
	ok &= expect(read_file(blocked_parent) == "not a directory\n",
	             "invalid output directory remains unchanged");

	fs::remove_all(root, error);
	ok &= expect(!error, "remove generator test directory");
	return ok ? 0 : 1;
}
