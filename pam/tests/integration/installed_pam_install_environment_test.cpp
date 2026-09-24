#include <cerrno>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <stdlib.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

namespace fs = std::filesystem;

namespace {

	class Cleanup {
	public:
		explicit Cleanup(fs::path root)
		    : root_(std::move(root)) {}

		Cleanup(const Cleanup &)                     = delete;
		auto operator=(const Cleanup &) -> Cleanup & = delete;
		Cleanup(Cleanup &&)                          = delete;
		auto operator=(Cleanup &&) -> Cleanup &      = delete;

		~Cleanup() {
			std::error_code error;
			fs::remove_all(root_, error);
		}

	private:
		fs::path root_;
	};

	auto Require(bool condition, const std::string &message) -> void {
		if (!condition) {
			throw std::runtime_error(message);
		}
	}

	auto RunHarness(const std::vector<std::string> &args, const fs::path &destdir) -> int {
		const pid_t child = fork();
		Require(child >= 0, "fork failed");
		if (child == 0) {
			if (setenv("DESTDIR", destdir.c_str(), 1) != 0 ||
			    setenv("CMAKE_INSTALL_MODE", "REL_SYMLINK", 1) != 0) {
				_exit(126);
			}
			std::vector<std::string> command = {"bash"};
			for (std::size_t index = 1; index < args.size(); ++index) {
				command.push_back(args.at(index));
			}
			std::vector<char *> arguments;
			arguments.reserve(command.size() + 1);
			for (auto &argument : command) {
				arguments.push_back(argument.data());
			}
			arguments.push_back(nullptr);
			execvp("bash", arguments.data());
			_exit(127);
		}
		int status = 0;
		while (waitpid(child, &status, 0) < 0) {
			Require(errno == EINTR, "waitpid failed");
		}
		Require(WIFEXITED(status), "harness terminated by signal");  // NOLINT(misc-include-cleaner)
		return WEXITSTATUS(status);                                  // NOLINT(misc-include-cleaner)
	}

	auto EmptyDirectory(const fs::path &path) -> bool {
		return fs::directory_iterator(path) == fs::directory_iterator();
	}

}  // namespace

auto main(int argc, char **argv) -> int {
	if (argc != 13) {
		std::cerr << "usage: installed_pam_install_environment_test HARNESS PATH_VALIDATOR "
		             "STATE_VERIFIER BUILD PREFIX "
		             "PAM_DIR HELPER_DIR CONFIG_DIR USER_MODELS_DIR DRIVER RUNTIME_DRIVER PROBE\n";
		return 2;
	}
	try {
		std::vector<std::string> args;
		for (const auto *argument : std::span(argv, static_cast<std::size_t>(argc))) {
			args.emplace_back(argument);
		}
		std::string path_template = "/tmp/howdy-install-environment.XXXXXX";
		const char *created       = mkdtemp(path_template.data());
		Require(created != nullptr, "cannot create fixture directory");
		const fs::path fixture = created;
		const Cleanup  cleanup(fixture);
		Require(chmod(fixture.c_str(), 0777) == 0, "cannot set fixture permissions");
		const auto hostile_destdir = fixture / "destdir";
		const auto redirect_target = fixture / "redirect-target";
		fs::create_directory(hostile_destdir);
		fs::create_directory(redirect_target);
		Require(chmod(hostile_destdir.c_str(), 0777) == 0 &&
		            chmod(redirect_target.c_str(), 0777) == 0,
		        "cannot set hostile directory permissions");
		fs::create_symlink(redirect_target, hostile_destdir / "opt");

		const int status = RunHarness(args, hostile_destdir);
		if (status == 77) {
			return 77;
		}
		Require(status == 0, "privileged harness failed with status " + std::to_string(status));
		Require(!fs::exists(args.at(5)) && !fs::is_symlink(fs::symlink_status(args.at(5))),
		        "trusted isolated installation remains after harness cleanup");
		Require(fs::is_directory(hostile_destdir), "hostile DESTDIR fixture was removed");
		Require(fs::is_symlink(hostile_destdir / "opt") &&
		            fs::read_symlink(hostile_destdir / "opt") == redirect_target,
		        "hostile DESTDIR fixture symlink changed");
		for (const auto &entry : fs::directory_iterator(hostile_destdir)) {
			Require(entry.path() == hostile_destdir / "opt",
			        "installation wrote below hostile DESTDIR: " + entry.path().string());
		}
		Require(EmptyDirectory(redirect_target), "installation followed hostile DESTDIR symlink");
		std::cout << "Privileged install harness environment isolation: passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
