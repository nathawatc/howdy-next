#include "integration/process.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <pwd.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/wait.h>

namespace {

	auto Require(bool condition, const std::string &message) -> void {
		if (!condition) {
			throw std::runtime_error(message);
		}
	}

	class Session {
	public:
		Session(const std::string &driver, const std::string &binary, const std::string &completion,
		        const std::vector<std::string> &emulator) {
			std::vector<std::string> command = {"bash", driver, binary, completion};
			command.insert(command.end(), emulator.begin(), emulator.end());
			std::vector<char *> command_argv;
			command_argv.reserve(command.size() + 1);
			for (auto &argument : command) {
				command_argv.push_back(argument.data());
			}
			command_argv.push_back(nullptr);
			std::array<int, 2> input{};
			std::array<int, 2> output{};
			Require(pipe2(input.data(), O_CLOEXEC) == 0, "cannot create completion input pipe");
			if (pipe2(output.data(), O_CLOEXEC) != 0) {
				close(input.at(0));
				close(input.at(1));
				throw std::runtime_error("cannot create completion output pipe");
			}
			pid_ = fork();
			if (pid_ < 0) {
				for (const auto fd : {input.at(0), input.at(1), output.at(0), output.at(1)}) {
					close(fd);
				}
				throw std::runtime_error("cannot fork completion driver");
			}
			if (pid_ == 0) {
				close(input.at(1));
				close(output.at(0));
				if (dup2(input.at(0), STDIN_FILENO) < 0 || dup2(output.at(1), STDOUT_FILENO) < 0) {
					_exit(126);
				}
				close(input.at(0));
				close(output.at(1));
				execvp("bash", command_argv.data());
				_exit(127);
			}
			close(input.at(0));
			close(output.at(1));
			write_fd_ = input.at(1);
			read_fd_  = output.at(0);
		}

		Session(const Session &)                     = delete;
		auto operator=(const Session &) -> Session & = delete;
		Session(Session &&)                          = delete;
		auto operator=(Session &&) -> Session &      = delete;

		~Session() {
			if (write_fd_ >= 0) {
				close(write_fd_);
			}
			if (read_fd_ >= 0) {
				close(read_fd_);
			}
			if (pid_ > 0) {
				int status = 0;
				while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
				}
			}
		}

		auto Query(const std::vector<std::string> &words) -> std::vector<std::string> {
			Send(std::to_string(words.size()));
			for (const auto &word : words) {
				Send(word);
			}
			const auto count = std::stoul(Receive());
			Require(count < 1000, "completion driver returned excessive results");
			std::vector<std::string> replies;
			replies.reserve(count);
			for (std::size_t index = 0; index < count; ++index) {
				replies.push_back(Receive());
			}
			return replies;
		}

		auto Finish() -> void {
			close(write_fd_);
			write_fd_  = -1;
			int status = 0;
			while (waitpid(pid_, &status, 0) < 0) {
				Require(errno == EINTR, "cannot wait for completion driver");
			}
			pid_ = -1;
			Require(WIFEXITED(status) && WEXITSTATUS(status) == 0,  // NOLINT(misc-include-cleaner)
			        "completion driver exited unsuccessfully");
		}

	private:
		auto Send(std::string token) const -> void {
			token.push_back('\0');
			std::size_t offset = 0;
			while (offset < token.size()) {
				const auto remaining = std::span(token.data(), token.size()).subspan(offset);
				const auto written   = write(write_fd_, remaining.data(), remaining.size());
				if (written < 0 && errno == EINTR) {
					continue;
				}
				Require(written > 0, "cannot write to completion driver");
				offset += static_cast<std::size_t>(written);
			}
		}

		[[nodiscard]] auto Receive() const -> std::string {
			std::string token;
			char        character = 0;
			for (;;) {
				const auto count = read(read_fd_, &character, 1);
				if (count < 0 && errno == EINTR) {
					continue;
				}
				Require(count == 1, "completion driver stopped before replying");
				if (character == '\0') {
					return token;
				}
				token.push_back(character);
			}
		}

		pid_t pid_      = -1;
		int   write_fd_ = -1;
		int   read_fd_  = -1;
	};

	auto Show(const std::vector<std::string> &values) -> std::string {
		std::string output;
		for (const auto &value : values) {
			output += "[" + value + "] ";
		}
		return output;
	}

	auto
	Check(Session &session, const std::string &label,
	      const std::vector<std::string> &words,  // NOLINT(bugprone-easily-swappable-parameters)
	      const std::vector<std::string> &expected = {}, bool contains = false) -> void {
		const auto actual = session.Query(words);
		const bool passed = contains ? std::ranges::find(actual, expected.at(0)) != actual.end()
		                             : actual == expected;
		Require(passed, label + ": expected " + Show(expected) + ", got " + Show(actual));
	}

}  // namespace

auto main(int argc, char **argv) -> int {
	if (argc < 4) {
		std::cerr << "usage: bash_completion_test DRIVER HOWDY COMPLETION\n";
		return 2;
	}
	try {
		const auto args   = howdy::test::integration::CopyArguments(argc, argv);
		const auto syntax = howdy::test::integration::RunProcess({"bash", "-n", args.at(3)});
		Require(syntax.status == 0, "completion syntax error: " + syntax.output);
		const auto *account = getpwuid(getuid());
		Require(account != nullptr, "cannot resolve current user");
		const std::string user = account->pw_name;
		Session session(args.at(1), args.at(2), args.at(3), {args.begin() + 4, args.end()});

		Check(session, "commands", {"howdy", ""},
		      {"add", "clear", "config", "disable", "download-models", "list", "remove", "set",
		       "snapshot", "test", "version"});
		Check(session, "command", {"howdy", "dis"}, {"disable"});
		Check(session, "short option before command", {"howdy", "-y", "a"}, {"add"});
		Check(session, "plain option before command", {"howdy", "--plain", "l"}, {"list"});
		Check(session, "combined options before command", {"howdy", "--plain", "-y", "a"}, {"add"});
		Check(session, "short user option before command", {"howdy", "-U", "alice", "t"}, {"test"});
		Check(session, "long user option before command", {"howdy", "--user", "alice", "t"},
		      {"test"});
		const std::vector<std::string> short_options = {"-U", "--user", "--plain",
		                                                "-y", "-h",     "--help"};
		const std::vector<std::string> long_options  = {"--user", "--plain", "--help"};
		Check(session, "short options", {"howdy", "-"}, short_options);
		Check(session, "long options", {"howdy", "--"}, long_options);
		Check(session, "post-command short options", {"howdy", "add", "-"}, short_options);
		Check(session, "post-command long options", {"howdy", "add", "--"}, long_options);
		Check(session, "test device option", {"howdy", "test", "--d"}, {"--device"});
		Check(session, "long option", {"howdy", "--p"}, {"--plain"});
		Check(session, "short user value", {"howdy", "-U", user}, {user});
		Check(session, "user value", {"howdy", "--user", user}, {user});
		Check(session, "empty user value", {"howdy", "--user", ""}, {user}, true);
		Check(session, "post-command short user value", {"howdy", "add", "-U", ""}, {user}, true);
		Check(session, "post-command user value", {"howdy", "add", "--user", ""}, {user}, true);
		Check(session, "empty disable value", {"howdy", "disable", ""}, {"false", "true"});
		Check(session, "disable false value", {"howdy", "disable", "f"}, {"false"});
		Check(session, "disable true value", {"howdy", "disable", "t"}, {"true"});
		Check(session, "set key", {"howdy", "set", ""}, {"detection_notice"}, true);
		Check(session, "set key prefix", {"howdy", "set", "sfa"},
		      {"sface_metric", "sface_threshold"});
		Check(session, "set boolean value", {"howdy", "set", "detection_notice", ""},
		      {"false", "true"});
		Check(session, "set choice values", {"howdy", "set", "sface_metric", ""},
		      {"cosine", "l2", "l2norm"});

		Check(session, "yes option excludes command", {"howdy", "-y", "dis"});
		Check(session, "plain option excludes command", {"howdy", "--plain", "dis"});
		Check(session, "combined options exclude command", {"howdy", "--plain", "-y", "l"});
		Check(session, "short user option excludes command", {"howdy", "-U", "alice", "dis"});
		Check(session, "long user option excludes command", {"howdy", "--user", "alice", "dis"});
		Check(session, "typed yes option", {"howdy", "disable", "-y", ""});
		Check(session, "typed plain option", {"howdy", "disable", "--plain", ""});
		Check(session, "typed user option", {"howdy", "disable", "-U", "alice", ""});
		Check(session, "pre-command incompatible option", {"howdy", "-y", "disable", ""});
		Check(session, "help terminates completion", {"howdy", "add", "--help", "-"});
		Check(session, "unknown command option", {"howdy", "list", "--bogus", "-"});
		Check(session, "foreign command option", {"howdy", "list", "--device", "/dev/video0", "-"});
		Check(session, "unknown option after command option",
		      {"howdy", "test", "--device", "/dev/video0", "--bogus", "-"});
		Check(session, "duplicate command option",
		      {"howdy", "test", "--device", "/dev/video0", "--device", "/dev/video1", ""});
		Check(session, "surplus positional", {"howdy", "list", "bob", "-"});
		Check(session, "surplus positional after terminator", {"howdy", "list", "--", "bob", "-"});
		Check(session, "surplus positional after global option",
		      {"howdy", "list", "--plain", "bob", "-"});
		Check(session, "surplus positional after command option",
		      {"howdy", "test", "--device", "/dev/video0", "extra", "-"});
		Check(session, "empty committed device value", {"howdy", "test", "--device", "", "-"});
		Check(session, "empty committed user value", {"howdy", "-U", "", "list", "-"});
		Check(session, "disable help options", {"howdy", "disable", "-"}, {"-h", "--help"});
		Check(session, "disable rejects short user value", {"howdy", "disable", "-U", ""});
		Check(session, "disable rejects user value", {"howdy", "disable", "--user", ""});
		Check(session, "end-of-options suppresses user", {"howdy", "add", "--", "-U", ""});
		Check(session, "used test device option",
		      {"howdy", "test", "--device", "/dev/video0", "--d"});
		Check(session, "set numeric value", {"howdy", "set", "timeout", ""});
		Check(session, "set floating-point value", {"howdy", "set", "sface_threshold", ""});
		Check(session, "version has no values", {"howdy", "version", ""});
		Check(session, "help disables completion", {"howdy", "config", "--help", "f"});
		Check(session, "help suppresses user", {"howdy", "--help", "-U", ""});
		session.Finish();
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
