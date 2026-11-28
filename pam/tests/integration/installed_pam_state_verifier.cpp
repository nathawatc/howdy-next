#include "integration/process.hpp"
#include "model_assets/opencv_model_manifest.hpp"
#include "protocol/runtime_descriptor_message.hpp"
#include "support/bit_flags.hpp"
#include "support/scoped_fd.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

namespace fs = std::filesystem;

namespace {
	namespace message = howdy::native::runtime_descriptor_message;
	using howdy::native::ScopedFd;
	using howdy::test::Require;

	auto ParseId(const std::string &value) -> unsigned {
		std::size_t parsed = 0;
		const auto  result = std::stoul(value, &parsed);
		Require(parsed == value.size(), "invalid numeric identity: " + value);
		return static_cast<unsigned>(result);
	}

	auto Inspect(const fs::path &path, bool directory, unsigned mode) -> void {
		struct stat info{};
		Require(lstat(path.c_str(), &info) == 0, "cannot inspect: " + path.string());
		Require((directory ? S_ISDIR(info.st_mode) : S_ISREG(info.st_mode)) && info.st_uid == 0 &&
		            info.st_gid == 0 && (static_cast<unsigned>(info.st_mode) & 07777U) == mode,
		        "wrong type, owner, or mode: " + path.string());
	}

	auto CannotWrite(const fs::path &path, unsigned uid, unsigned gid) -> bool {
		const auto result = howdy::test::integration::RunProcess({
		    "setpriv",
		    "--reuid",
		    std::to_string(uid),
		    "--regid",
		    std::to_string(gid),
		    "--init-groups",
		    "test",
		    "!",
		    "-w",
		    path.string(),
		});
		return result.status == 0;
	}

	// NOLINTBEGIN(bugprone-easily-swappable-parameters)
	auto RunAsUser(const fs::path &program, const std::vector<std::string> &arguments, unsigned uid,
	               unsigned gid, int close_fd = -1) -> howdy::test::integration::ProcessResult {
		// NOLINTEND(bugprone-easily-swappable-parameters)
		std::vector<std::string> command = {
		    "setpriv",           "--reuid",       std::to_string(uid), "--regid",
		    std::to_string(gid), "--init-groups", program.string(),
		};
		command.insert(command.end(), arguments.begin(), arguments.end());
		return howdy::test::integration::RunProcess(std::move(command), {}, close_fd);
	}

	auto ReadAll(int fd) -> std::string {
		std::string            content;
		std::array<char, 4096> buffer{};
		off_t                  offset = 0;
		for (;;) {
			const ssize_t count = pread(fd, buffer.data(), buffer.size(), offset);
			Require(count >= 0, "cannot read a received descriptor");
			if (count == 0) {
				return content;
			}
			content.append(buffer.data(), static_cast<std::size_t>(count));
			offset += count;
		}
	}

	auto ReadFile(const fs::path &path) -> std::string {
		std::ifstream input(path, std::ios::binary);
		Require(input.good(), "cannot read: " + path.string());
		return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	}

	// A received descriptor must be close-on-exec, open read-only, and name a regular file with
	// exactly the bytes of `source`.
	auto RequireDescriptorFor(const ScopedFd &fd, const fs::path &source, const char *label)
	    -> void {
		const int   descriptor_flags = fcntl(fd.Get(), F_GETFD);
		const int   status_flags     = fcntl(fd.Get(), F_GETFL);
		struct stat info{};
		Require(fd.Valid() && descriptor_flags >= 0 &&
		            howdy::native::HasAnyBits(descriptor_flags, FD_CLOEXEC) && status_flags >= 0 &&
		            howdy::native::MaskedBits(status_flags, O_ACCMODE) ==
		                howdy::native::BitRepresentation(O_RDONLY) &&
		            fstat(fd.Get(), &info) == 0 && S_ISREG(info.st_mode),
		        std::string(label) + " descriptor has the wrong flags or type");
		Require(ReadAll(fd.Get()) == ReadFile(source),
		        std::string(label) + " descriptor does not match " + source.string());
	}

	struct HelperRun {
		int                      status = -1;
		std::string              output;
		message::ReceivedMessage received;
		message::ReceiveStatus   after = message::ReceiveStatus::kError;
	};

	// Runs the helper's `open` as the user with one end of a socket pair at descriptor 3, as
	// PAM does, and collects its one message and what follows it.
	// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
	auto RunOpenWithSocket(const fs::path &helper, const std::string &user, unsigned uid,
	                       unsigned gid) -> HelperRun {
		std::array<int, 2> sockets{-1, -1};
		std::array<int, 2> output{-1, -1};
		Require(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets.data()) == 0 &&
		            pipe2(output.data(), O_CLOEXEC) == 0,
		        "cannot create helper probe channels");
		ScopedFd    pam_end(sockets.at(0));
		ScopedFd    helper_end(sockets.at(1));
		ScopedFd    output_read(output.at(0));
		ScopedFd    output_write(output.at(1));
		std::string uid_text = std::to_string(uid);
		std::string gid_text = std::to_string(gid);
		std::string program  = helper.string();
		std::string command  = "open";
		std::string name     = user;
		const pid_t child    = fork();
		Require(child >= 0, "cannot fork helper probe");
		if (child == 0) {
			if (dup2(helper_end.Get(), message::kHelperSocketFd) < 0 ||
			    dup2(output_write.Get(), STDOUT_FILENO) < 0 ||
			    dup2(output_write.Get(), STDERR_FILENO) < 0) {
				_exit(126);
			}
			std::string            setpriv = "setpriv";
			std::string            reuid   = "--reuid";
			std::string            regid   = "--regid";
			std::string            groups  = "--init-groups";
			std::array<char *, 10> argv{
			    setpriv.data(), reuid.data(),   uid_text.data(), regid.data(), gid_text.data(),
			    groups.data(),  program.data(), command.data(),  name.data(),  nullptr,
			};
			execvp(argv.front(), argv.data());
			_exit(127);
		}
		helper_end.Reset();
		output_write.Reset();
		HelperRun              run;
		std::array<char, 4096> buffer{};
		for (;;) {
			const ssize_t count = read(output_read.Get(), buffer.data(), buffer.size());
			if (count > 0) {
				run.output.append(buffer.data(), static_cast<std::size_t>(count));
			} else if (count == 0 || errno != EINTR) {
				break;
			}
		}
		int status = 0;
		while (waitpid(child, &status, 0) < 0) {
			Require(errno == EINTR, "cannot wait for helper probe");
		}
		run.status = WIFEXITED(status) ? WEXITSTATUS(status) : 128;  // NOLINT(misc-include-cleaner)
		run.received = message::ReceiveMessage(pam_end.Get());
		run.after    = message::ReceiveMessage(pam_end.Get()).status;
		return run;
	}

	// NOLINTBEGIN(bugprone-easily-swappable-parameters)
	auto VerifyHelperProbes(const fs::path &helper, const std::string &user, unsigned uid,
	                        unsigned gid, const fs::path &config, const fs::path &model) -> int {
		// NOLINTEND(bugprone-easily-swappable-parameters)
		const auto invalid = RunAsUser(helper, {"open", "../invalid-e2e-user"}, uid, gid);
		Require(invalid.status != 0, "invalid username unexpectedly succeeded");
		if (invalid.output.contains("must be installed setuid root")) {
			return 77;
		}
		Require(invalid.output.contains("Invalid user name"),
		        "setuid preflight did not reach username validation: " + invalid.output);
		const auto wrong_user = RunAsUser(helper, {"open", "root"}, uid, gid);
		Require(wrong_user.status != 0, "helper opened runtime files for a different username");
		const auto missing_fd = RunAsUser(helper, {"open", user}, uid, gid, 3);
		Require(missing_fd.status != 0, "direct helper open succeeded without descriptor 3");
		Require(missing_fd.output ==
		            "howdy-auth-helper requires an inherited descriptor socket on descriptor 3\n",
		        "direct helper missing-fd error mismatch: " + missing_fd.output);

		const auto run = RunOpenWithSocket(helper, user, uid, gid);
		Require(run.status == 0 && run.output.empty(),
		        "helper open failed or wrote output: " + run.output);
		Require(run.received.status == message::ReceiveStatus::kMessage &&
		            run.received.model_state == message::ModelState::kReady,
		        "helper open did not send a ready descriptor message");
		Require(run.after == message::ReceiveStatus::kEndOfFile,
		        "helper open sent more than its one message");
		RequireDescriptorFor(run.received.config, config, "config");
		RequireDescriptorFor(run.received.model, model, "model");
		return 0;
	}

	auto VerifyInstalled(const fs::path &helper, const fs::path &compare,
	                     const fs::path &config_dir, const fs::path &config, const fs::path &models,
	                     unsigned selected_uid, unsigned selected_gid) -> void {
		Inspect(helper, false, 04755);
		Inspect(compare, false, 0755);
		Inspect(config_dir, true, 0750);
		Inspect(config, false, 0640);
		Inspect(models, true, 0750);
		for (auto parent = helper.parent_path(); !parent.empty(); parent = parent.parent_path()) {
			Require(CannotWrite(parent, selected_uid, selected_gid),
			        "helper parent writable by selected user: " + parent.string());
			if (parent == "/") {
				break;
			}
		}
	}

	auto ReadLines(const fs::path &path) -> std::vector<std::string> {
		std::ifstream input(path);
		Require(input.good(), "cannot read: " + path.string());
		std::vector<std::string> lines;
		for (std::string line; std::getline(input, line);) {
			lines.push_back(std::move(line));
		}
		Require(!input.bad(), "read failed: " + path.string());
		return lines;
	}

	auto ConfigureFixture(const fs::path &paths_header, const fs::path &config,
	                      const fs::path &models, const fs::path &helper, const fs::path &compare)
	    -> void {
		const auto paths = ReadLines(paths_header);
		for (const auto &[key, value] : {
		         std::pair{"kConfiguredConfigPath", config.string()},
		         std::pair{"kConfiguredUserModelsDir", models.string()},
		         std::pair{"kAuthHelperPath", helper.string()},
		         std::pair{"kCompareProcessPath", compare.string()},
		     }) {
			const auto expected =
			    "inline constexpr auto " + std::string(key) + " = \"" + value + "\";";
			Require(std::ranges::find(paths, expected) != paths.end(),
			        "compiled path mismatch: " + std::string(key));
		}
		auto lines = ReadLines(config);
		for (const auto &[key, value] : {
		         std::pair{"disabled", "true"},
		         std::pair{"detection_notice", "true"},
		         std::pair{"no_confirmation", "false"},
		         std::pair{"abort_if_ssh", "false"},
		         std::pair{"abort_if_lid_closed", "false"},
		     }) {
			const std::string prefix = std::string(key) + " = ";
			unsigned          count  = 0;
			for (auto &line : lines) {
				if (line.starts_with(prefix)) {
					line = prefix + value;
					++count;
				}
			}
			Require(count == 1,
			        "expected one " + std::string(key) + " setting in " + config.string());
		}
		std::ofstream output(config, std::ios::trunc);
		Require(output.good(), "cannot update config: " + config.string());
		for (const auto &line : lines) {
			output << line << '\n';
		}
		output.flush();
		Require(output.good(), "cannot write config: " + config.string());
	}

	auto WriteModel(const fs::path &path, const std::string &generation, const std::string &first,
	                const std::string &second) -> void {
		std::ofstream output(path, std::ios::trunc);
		Require(output.good(), "cannot write source model: " + path.string());
		output << R"([{"id":0,"time":)" << generation
		       << R"(,"label":"e2e","backend":"opencv_dnn_sface","metric":"cosine","model":")"
		       << howdy::native::kSfaceModelDescriptor.filename << R"(","data":[[)" << first << ','
		       << second << "]]}]\n";
		output.flush();
		Require(output.good(), "cannot write source model: " + path.string());
	}

	// Checks this verifier's own descriptor plumbing without root: a file sent over a socket pair
	// with the protocol's encoder arrives as a descriptor RequireDescriptorFor accepts.
	auto SelfTest() -> void {
		const auto created = howdy::test::CreateTempDirectory("howdy-e2e-state-verifier.");
		if (!created.has_value()) {
			Require(false, "cannot create state-verifier fixture");
			return;
		}
		const fs::path                       &root = *created;
		const howdy::test::TemporaryDirectory cleanup(root);
		const auto                            file = root / "config.ini";
		std::ofstream(file) << "[core]\ndisabled = true\n";
		std::array<int, 2> sockets{-1, -1};
		Require(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets.data()) == 0,
		        "cannot create self-test socket pair");
		const ScopedFd sender(sockets.at(0));
		const ScopedFd receiver(sockets.at(1));
		const ScopedFd source(open(file.c_str(), O_RDONLY | O_CLOEXEC));
		Require(message::SendMessage(sender.Get(),
		                             {
		                                 .model_state = message::ModelState::kMissing,
		                                 .config_fd   = source.Get(),
		                             }),
		        "cannot send self-test message");
		const auto received = message::ReceiveMessage(receiver.Get());
		Require(received.status == message::ReceiveStatus::kMessage,
		        "self-test message did not arrive");
		RequireDescriptorFor(received.config, file, "self-test config");
		std::cout << "State verifier: descriptor checks passed\n";
	}

}  // namespace

auto main(int argc, char **argv) -> int {
	const auto args = howdy::test::integration::CopyArguments(argc, argv);
	try {
		if (args.size() == 2 && args.at(1) == "--self-test") {
			SelfTest();
		} else if (args.size() == 9 && args.at(1) == "--installed") {
			VerifyInstalled(args.at(2), args.at(3), args.at(4), args.at(5), args.at(6),
			                ParseId(args.at(7)), ParseId(args.at(8)));
		} else if (args.size() == 7 && args.at(1) == "--configure") {
			ConfigureFixture(args.at(2), args.at(3), args.at(4), args.at(5), args.at(6));
		} else if (args.size() == 6 && args.at(1) == "--write-model") {
			WriteModel(args.at(2), args.at(3), args.at(4), args.at(5));
		} else if (args.size() == 8 && args.at(1) == "--helper-probes") {
			return VerifyHelperProbes(args.at(2), args.at(3), ParseId(args.at(4)),
			                          ParseId(args.at(5)), args.at(6), args.at(7));
		} else {
			std::cerr << "invalid state-verifier arguments\n";
			return 2;
		}
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
