#include "paths.hpp"
#include "protocol/runtime_descriptor_message.hpp"
#include "runtime/auth_helper_process.hpp"
#include "runtime/runtime_session.hpp"
#include "runtime/runtime_session_process_test_support.hpp"
#include "runtime/runtime_session_test_groups.hpp"
#include "support/bit_flags.hpp"
#include "support/process_test_support.hpp"
#include "support/scoped_fd.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/stat.h>

namespace {

	namespace message = howdy::native::runtime_descriptor_message;
	using howdy::pam::RuntimeDescriptorSet;
	using howdy::pam::auth_helper_process::OpenRuntimeDescriptors;
	using HelperOperations = howdy::pam::auth_helper_process::Operations;
	using howdy::test::Expect;
	using howdy::test::runtime_session::Logged;
	using howdy::test::runtime_session::SelfExecContext;
	using howdy::test::runtime_session::SelfExecOperations;
	using message::ModelState;

	// Records every spawn-setup operation without spawning anything.
	struct RecordingContext {
		std::vector<std::string> calls;
		std::vector<std::string> argv;
		std::vector<std::string> environment;
		std::array<int, 2>       socket_fds{30, 31};
		std::string              fail_at;
		int                      next_duplicate = 40;
		std::vector<int>         closed;
	};

	auto Fails(RecordingContext &context, std::string_view call) -> int {
		context.calls.emplace_back(call);
		return context.fail_at == call ? EIO : 0;
	}

	auto OpenMode(int flags) -> std::string_view {
		if (flags == O_RDONLY) {
			return "r";
		}
		return flags == O_WRONLY ? "w" : "?";
	}

	// NOLINTBEGIN(bugprone-easily-swappable-parameters)
	auto RecordingOperations(RecordingContext *context) -> HelperOperations {
		return {
		    .context    = context,
		    .socketpair = [](void *raw, int domain, int type, int protocol, int *fds) -> int {
			    auto      &recording = *static_cast<RecordingContext *>(raw);
			    const bool expected =
			        domain == AF_UNIX &&
			        type == howdy::native::CombineBitsAs<int>(SOCK_SEQPACKET, SOCK_CLOEXEC) &&
			        protocol == 0;
			    if (Fails(recording, expected ? "socketpair" : "socketpair(unexpected)") != 0) {
				    errno = EIO;
				    return -1;
			    }
			    const std::span<int, 2> out(fds, 2);
			    out.front() = recording.socket_fds.at(0);
			    out.back()  = recording.socket_fds.at(1);
			    return 0;
		    },
		    .duplicate_fd = [](void *raw, int fd, int minimum_fd) -> int {
			    auto &recording = *static_cast<RecordingContext *>(raw);
			    if (Fails(recording,
				          "dup " + std::to_string(fd) + " >= " + std::to_string(minimum_fd)) != 0) {
				    errno = EIO;
				    return -1;
			    }
			    return recording.next_duplicate++;
		    },
		    .actions_init = [](void *raw, posix_spawn_file_actions_t *) -> int {
			    return Fails(*static_cast<RecordingContext *>(raw), "init");
		    },
		    .actions_addopen = [](void *raw, posix_spawn_file_actions_t *, int fd, const char *path,
			                      int flags) -> int {
			    return Fails(*static_cast<RecordingContext *>(raw),
				             "open " + std::to_string(fd) + " " + std::string(path) + " " +
				                 std::string(OpenMode(flags)));
		    },
		    .actions_adddup2 = [](void *raw, posix_spawn_file_actions_t *, int source,
			                      int target) -> int {
			    return Fails(*static_cast<RecordingContext *>(raw),
				             "dup2 " + std::to_string(source) + " -> " + std::to_string(target));
		    },
		    .actions_addclosefrom = [](void *raw, posix_spawn_file_actions_t *, int from) -> int {
			    return Fails(*static_cast<RecordingContext *>(raw),
				             "closefrom " + std::to_string(from));
		    },
		    .actions_destroy = [](void *raw, posix_spawn_file_actions_t *) -> int {
			    static_cast<RecordingContext *>(raw)->calls.emplace_back("destroy");
			    return 0;
		    },
		    .spawn = [](const howdy::pam::auth_helper_process::SpawnRequest &request) -> int {
			    auto &recording = *static_cast<RecordingContext *>(request.context);
			    for (const char *argument : request.argv) {
				    if (argument == nullptr) {
					    break;
				    }
				    recording.argv.emplace_back(argument);
			    }
			    for (const char *entry : request.envp) {
				    if (entry == nullptr) {
					    break;
				    }
				    recording.environment.emplace_back(entry);
			    }
			    recording.calls.emplace_back("spawn");
			    return EACCES;  // stop here: nothing to exchange with
		    },
		    .close = [](void *raw, int fd) -> int {
			    static_cast<RecordingContext *>(raw)->closed.push_back(fd);
			    return 0;
		    },
		};
	}

	// NOLINTEND(bugprone-easily-swappable-parameters)

	auto TestSpawnSetup() -> bool {
		RecordingContext     context;
		RuntimeDescriptorSet descriptors;
		const bool           opened =
		    OpenRuntimeDescriptors("alice", &descriptors, RecordingOperations(&context));
		const std::vector<std::string> expected{
		    "socketpair",
		    "init",
		    "open 0 /dev/null r",
		    "open 1 /dev/null w",
		    "open 2 /dev/null w",
		    "dup2 31 -> 3",
		    "closefrom 4",
		    "spawn",
		    "destroy",
		};
		bool ok = Expect(!opened && context.calls == expected,
		                 "the helper gets /dev/null stdio, its socket at fd 3, and nothing else");
		ok &= Expect(context.argv == std::vector<std::string>{kAuthHelperPath, "open", "alice"} &&
		                 context.environment.empty(),
		             "the helper runs `open` for the user with an empty environment");
		ok &= Expect(context.closed.size() == 2, "a failed spawn closes both socket ends");
		return ok;
	}

	// A socket end the kernel puts at fd 3 or below is moved above it first.
	auto TestSocketEndsRaised() -> bool {
		RecordingContext context;
		context.socket_fds = {3, 2};
		RuntimeDescriptorSet descriptors;
		(void)OpenRuntimeDescriptors("alice", &descriptors, RecordingOperations(&context));
		const std::vector<std::string> expected_prefix{
		    "socketpair",
		    "dup 3 >= 4",
		    "dup 2 >= 4",
		    "init",
		};
		return Expect(context.calls.size() > expected_prefix.size() &&
		                  std::equal(expected_prefix.begin(), expected_prefix.end(),
		                             context.calls.begin()),
		              "socket ends at or below fd 3 are raised above it") &&
		       Expect(std::ranges::find(context.calls, "dup2 41 -> 3") != context.calls.end(),
		              "the raised child end is the one placed at fd 3");
	}

	auto TestSpawnSetupFailures() -> bool {
		bool ok = true;
		for (const std::string step : {
		         "socketpair",
		         "init",
		         "open 0 /dev/null r",
		         "open 2 /dev/null w",
		         "dup2 31 -> 3",
		         "closefrom 4",
		     }) {
			RecordingContext context;
			context.fail_at = step;
			RuntimeDescriptorSet descriptors;
			const bool           opened =
			    OpenRuntimeDescriptors("alice", &descriptors, RecordingOperations(&context));
			const bool spawned = std::ranges::find(context.calls, "spawn") != context.calls.end();
			const bool closed  = step == "socketpair" || context.closed.size() == 2;
			ok &= Expect(!opened && !spawned && closed,
			             "a failed " + step + " stops before spawning and closes the socket");
		}
		return ok;
	}

	struct Exchange {
		bool                 opened = false;
		RuntimeDescriptorSet descriptors;
		SelfExecContext      context;
	};

	auto RunExchange(std::string_view          variant,
	                 std::chrono::milliseconds timeout = std::chrono::seconds(10)) -> Exchange {
		Exchange exchange;
		exchange.context.mode = {"--fake-helper", std::string(variant)};
		exchange.opened       = OpenRuntimeDescriptors("alice", &exchange.descriptors,
		                                               SelfExecOperations(&exchange.context),
		                                               std::chrono::steady_clock::now() + timeout);
		return exchange;
	}

	auto Reaped(const Exchange &exchange) -> bool {
		return exchange.context.spawned_pid > 0 &&
		       howdy::test::process::ChildReaped(exchange.context.spawned_pid);
	}

	auto CloseOnExec(int fd) -> bool {
		const int flags = fcntl(fd, F_GETFD);
		return flags >= 0 && howdy::native::HasAnyBits(flags, FD_CLOEXEC);
	}

	auto TestExchanges() -> bool {
		bool ok = true;

		const auto ready = RunExchange("ready");
		ok &= Expect(ready.opened && ready.descriptors.model_state == ModelState::kReady &&
		                 ready.descriptors.config.Valid() && ready.descriptors.model.Valid() &&
		                 CloseOnExec(ready.descriptors.config.Get()) &&
		                 CloseOnExec(ready.descriptors.model.Get()),
		             "a ready message gives close-on-exec config and model descriptors");
		ok &= Expect(Reaped(ready), "the helper is reaped after a ready message");

		const auto missing = RunExchange("missing");
		ok &= Expect(missing.opened && missing.descriptors.model_state == ModelState::kMissing &&
		                 missing.descriptors.config.Valid() && !missing.descriptors.model.Valid(),
		             "a missing model gives the config descriptor alone");

		const auto invalid = RunExchange("invalid");
		ok &=
		    Expect(invalid.opened && invalid.descriptors.model_state == ModelState::kInvalidStorage,
			       "invalid model storage is reported, with the config");

		const auto silent = RunExchange("silent");
		ok &= Expect(!silent.opened && Logged(silent.context, "without sending") && Reaped(silent),
		             "a helper that exits 0 without a message fails");

		const auto twice = RunExchange("twice");
		ok &= Expect(!twice.opened && Logged(twice.context, "more than one message") &&
		                 !twice.descriptors.config.Valid(),
		             "a second message fails and keeps nothing");

		const auto wrong = RunExchange("wrong-marker");
		ok &= Expect(!wrong.opened && Logged(wrong.context, "malformed"),
		             "a malformed message fails");

		const auto exit_one = RunExchange("ready-then-fail");
		ok &= Expect(exit_one.opened && Logged(exit_one.context, "exited abnormally"),
		             "the message decides; a failing exit status is only logged");

		const auto stalled = RunExchange("ready-then-stall", std::chrono::milliseconds(300));
		ok &= Expect(!stalled.opened && Logged(stalled.context, "timed out") && Reaped(stalled),
		             "a message without end of file times out and the helper is stopped");
		return ok;
	}

	// A host that auto-reaps SIGCHLD takes the helper's exit status; the message still decides.
	auto TestHostReapsHelper() -> bool {
		const howdy::test::process::ScopedSigchldAction ignored(SIG_IGN, 0);
		const auto                                      ready = RunExchange("ready");
		return Expect(ignored.Valid() && ready.opened &&
		                  ready.descriptors.model_state == ModelState::kReady,
		              "the helper's message decides when the host reaps it");
	}

}  // namespace

// Child mode: the helper's view of its descriptors, then a valid message so the exchange ends.
auto RunRuntimeSessionFd3Probe(const char *marker_path) -> int {
	struct stat status{};
	int         type   = -1;
	socklen_t   length = sizeof(type);
	const bool  socket =
	    fstat(message::kHelperSocketFd, &status) == 0 && S_ISSOCK(status.st_mode) &&
	    getsockopt(message::kHelperSocketFd, SOL_SOCKET, SO_TYPE, &type, &length) == 0 &&
	    type == SOCK_SEQPACKET;
	bool stdio_null = true;
	for (const int fd : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) {
		stdio_null &= fstat(fd, &status) == 0 && S_ISCHR(status.st_mode);
	}
	bool nothing_else = true;
	for (int fd = message::kHelperSocketFd + 1; fd < 64; ++fd) {
		nothing_else &= fcntl(fd, F_GETFD) < 0;
	}
	if (!socket || !stdio_null || !nothing_else) {
		return EXIT_FAILURE;
	}
	const howdy::native::ScopedFd marker(
	    open(marker_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
	const howdy::native::ScopedFd config(open("/proc/self/exe", O_RDONLY | O_CLOEXEC));
	return marker.Valid() && message::SendMessage(message::kHelperSocketFd,
	                                              {
	                                                  .model_state = ModelState::kMissing,
	                                                  .config_fd   = config.Get(),
	                                              })
			   ? EXIT_SUCCESS
			   : EXIT_FAILURE;
}

namespace {

	// With fd 3 free, the kernel hands the socketpair fd 3; the real spawn must still give the
	// helper its own end there, stdio on /dev/null, and nothing above.
	auto TestRealDescriptorLayout() -> bool {
		const auto      marker = howdy::test::runtime_session::MarkerPath("fd3-probe");
		std::error_code error;
		std::filesystem::remove(marker, error);
		const howdy::native::ScopedFd saved(fcntl(3, F_DUPFD_CLOEXEC, 10));
		(void)close(3);
		SelfExecContext context;
		context.mode = {"--fd3-probe", marker.string()};
		RuntimeDescriptorSet descriptors;
		const bool           opened =
		    OpenRuntimeDescriptors("alice", &descriptors, SelfExecOperations(&context));
		if (saved.Valid()) {
			(void)dup2(saved.Get(), 3);
		}
		const bool marked = std::filesystem::exists(marker, error);
		std::filesystem::remove(marker, error);
		return Expect(opened && marked && descriptors.model_state == ModelState::kMissing,
		              "the helper sees its socket at fd 3, /dev/null stdio and nothing else");
	}

}  // namespace

auto RunRuntimeSessionSpawnTests() -> bool {
	bool ok = true;
	ok &= TestSpawnSetup();
	ok &= TestSocketEndsRaised();
	ok &= TestSpawnSetupFailures();
	ok &= TestExchanges();
	ok &= TestHostReapsHelper();
	ok &= TestRealDescriptorLayout();
	return ok;
}
