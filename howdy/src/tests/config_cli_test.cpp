#include "cli/config_cli.hpp"

#include <array>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <unistd.h>

#include <sys/stat.h>

namespace {

	auto write_file(const std::filesystem::path &path, const std::string &content) -> bool {
		std::ofstream out(path);
		if (!out.is_open()) {
			return false;
		}
		out << content;
		return out.good();
	}

	auto read_file(const std::filesystem::path &path) -> std::string {
		std::ifstream in(path);
		if (!in.is_open()) {
			return {};
		}
		return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
	}

	auto get_env_value(const char *name) -> std::optional<std::string> {
		const char *value = std::getenv(name);
		if (value == nullptr) {
			return std::nullopt;
		}
		return std::string(value);
	}

	struct EnvVarGuard {
		const char                *name;
		std::optional<std::string> previous;

		EnvVarGuard(const char *env_name, const std::string &value)
		    : name(env_name)
		    , previous(get_env_value(env_name)) {
			setenv(name, value.c_str(), 1);
		}

		EnvVarGuard(const EnvVarGuard &)                     = delete;
		auto operator=(const EnvVarGuard &) -> EnvVarGuard & = delete;

		~EnvVarGuard() {
			if (previous.has_value()) {
				setenv(name, previous->c_str(), 1);
				return;
			}
			unsetenv(name);
		}
	};

	struct StdoutRedirectGuard {
		int  saved_stdout = -1;
		bool active       = false;

		explicit StdoutRedirectGuard(const std::filesystem::path &path) {
			saved_stdout = dup(STDOUT_FILENO);
			if (saved_stdout < 0) {
				return;
			}

			const int output_fd =
			    open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
			if (output_fd < 0) {
				close(saved_stdout);
				saved_stdout = -1;
				return;
			}

			if (dup2(output_fd, STDOUT_FILENO) < 0) {
				close(output_fd);
				close(saved_stdout);
				saved_stdout = -1;
				return;
			}
			close(output_fd);
			active = true;
		}

		StdoutRedirectGuard(const StdoutRedirectGuard &)                     = delete;
		auto operator=(const StdoutRedirectGuard &) -> StdoutRedirectGuard & = delete;

		~StdoutRedirectGuard() {
			if (saved_stdout >= 0) {
				std::cout.flush();
				dup2(saved_stdout, STDOUT_FILENO);
				close(saved_stdout);
			}
		}

		[[nodiscard]] auto ok() const -> bool {
			return active;
		}
	};

	auto capture_config_main_stdout(const std::filesystem::path &path, int *exit_code) -> bool {
		StdoutRedirectGuard stdout_redirect(path);
		if (!stdout_redirect.ok()) {
			return false;
		}

		std::array<char *, 2> argv = {
		    const_cast<char *>("howdy-config"),
		    nullptr,
		};
		*exit_code = config_main(1, argv.data());
		return true;
	}

	auto expect(bool condition, const std::string &message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << "\n";
			return false;
		}
		return true;
	}

	auto path_reported_after(const std::string &output, const std::string &prefix)
	    -> std::filesystem::path {
		const auto start = output.find(prefix);
		if (start == std::string::npos) {
			return {};
		}
		const auto path_start = start + prefix.size();
		const auto path_end   = output.find('\n', path_start);
		return output.substr(path_start, path_end - path_start);
	}

}  // namespace

auto main() -> int {
	namespace fs = std::filesystem;

	bool            ok        = true;
	const auto      temp_root = fs::current_path() / "howdy-config-cli-test-work";
	std::error_code ec;
	fs::remove_all(temp_root, ec);
	fs::create_directories(temp_root, ec);
	ok &= expect(!ec, "create temp root");

	const auto config_path = temp_root / "config.ini";
	const auto editor_path = temp_root / "fake-editor";
	const auto output_path = temp_root / "stdout.txt";
	ok &= expect(write_file(config_path, "[core]\ndisabled = false\n"),
	             "write valid config baseline");
	ok &= expect(chmod(config_path.c_str(), 0600) == 0, "secure config file mode");
	ok &= expect(write_file(editor_path, "#!/bin/sh\nprintf '[core\\n' > \"$1\"\n"),
	             "write fake editor");
	ok &= expect(chmod(editor_path.c_str(), 0700) == 0, "make fake editor executable");

	EnvVarGuard editor_env("EDITOR", editor_path.string());
	EnvVarGuard config_env("HOWDY_CONFIG", config_path.string());

	int exit_code = 0;
	ok &= expect(capture_config_main_stdout(output_path, &exit_code), "capture config_main output");

	const auto                 output          = read_file(output_path);
	constexpr std::string_view recovery_prefix = "Edited config is invalid and was not installed: ";
	const auto reported_temp_path = path_reported_after(output, std::string(recovery_prefix));
	ok &= expect(exit_code == 1, "config_main aborts on invalid edited config");
	ok &= expect(!reported_temp_path.empty(), "config_main reports edited temp path");
	ok &= expect(fs::exists(reported_temp_path, ec) && !ec,
	             "config_main preserves invalid edited temp file");
	ok &= expect(read_file(reported_temp_path) == "[core\n",
	             "preserved edited temp file contains invalid config");

	fs::remove(reported_temp_path, ec);
	fs::remove_all(temp_root, ec);
	return ok ? 0 : 1;
}
