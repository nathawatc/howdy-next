#pragma once

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <sys/wait.h>

namespace howdy::test::integration {

	struct ProcessResult {
		int         status;
		std::string output;
	};

	inline auto CopyArguments(int argc, char **argv) -> std::vector<std::string> {
		std::vector<std::string> arguments;
		for (const auto *argument : std::span(argv, static_cast<std::size_t>(argc))) {
			arguments.emplace_back(argument);
		}
		return arguments;
	}

	inline auto RunProcess(std::vector<std::string>                                arguments,
	                       const std::vector<std::pair<std::string, std::string>> &environment = {},
	                       int close_fd = -1) -> ProcessResult {
		std::array<int, 2> pipe_fds{};
		if (pipe2(pipe_fds.data(), O_CLOEXEC) != 0) {
			throw std::runtime_error("pipe failed");
		}
		const pid_t child = fork();
		if (child < 0) {
			close(pipe_fds.at(0));
			close(pipe_fds.at(1));
			throw std::runtime_error("fork failed");
		}
		if (child == 0) {
			close(pipe_fds.at(0));
			for (const auto &[name, value] : environment) {
				if (setenv(name.c_str(), value.c_str(), 1) != 0) {
					_exit(126);
				}
			}
			if (dup2(pipe_fds.at(1), STDOUT_FILENO) < 0 ||
			    dup2(pipe_fds.at(1), STDERR_FILENO) < 0) {
				_exit(126);
			}
			close(pipe_fds.at(1));
			if (close_fd >= 0) {
				close(close_fd);
			}
			std::vector<char *> argv;
			argv.reserve(arguments.size() + 1);
			for (auto &argument : arguments) {
				argv.push_back(argument.data());
			}
			argv.push_back(nullptr);
			execvp(argv.front(), argv.data());
			_exit(127);
		}
		close(pipe_fds.at(1));
		std::string            output;
		std::array<char, 4096> buffer{};
		for (;;) {
			const ssize_t count = read(pipe_fds.at(0), buffer.data(), buffer.size());
			if (count > 0) {
				output.append(buffer.data(), static_cast<std::size_t>(count));
			} else if (count == 0) {
				break;
			} else if (errno != EINTR) {
				close(pipe_fds.at(0));
				throw std::runtime_error("read from child failed");
			}
		}
		close(pipe_fds.at(0));
		int status = 0;
		while (waitpid(child, &status, 0) < 0) {
			if (errno != EINTR) {
				throw std::runtime_error("waitpid failed");
			}
		}
		if (WIFEXITED(status)) {
			return {.status = WEXITSTATUS(status), .output = std::move(output)};
		}
		if (WIFSIGNALED(status)) {
			return {.status = 128 + WTERMSIG(status), .output = std::move(output)};
		}
		throw std::runtime_error("child exited unexpectedly");
	}

}  // namespace howdy::test::integration
