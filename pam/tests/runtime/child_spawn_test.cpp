#include "runtime/child_process.hpp"
#include "support/bit_flags.hpp"
#include "support/scoped_fd.hpp"
#include "test_support.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <spawn.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <sys/wait.h>

// HOWDY_HAVE_PIDFD_SPAWN comes from the build (pam_runtime) when the C library has pidfd_spawn;
// without it only the posix_spawn path is tested.

namespace {

	struct PidfdSpawnState {
		bool force_enosys = false;
		// When set, pidfd_getpid fails with it: for every pidfd, or, with
		// getpid_fails_children_only, only for a pidfd of another process than this one.
		int  getpid_error               = 0;
		bool getpid_fails_children_only = false;
		int  calls                      = 0;
	};

	auto SpawnState() -> PidfdSpawnState & {
		static PidfdSpawnState state;
		return state;
	}

}  // namespace

#ifdef HOWDY_HAVE_PIDFD_SPAWN
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
extern "C" auto TestPidfdSpawn(int *pidfd, const char *path,
                               const posix_spawn_file_actions_t *actions,
                               const posix_spawnattr_t *attributes, char *const argv[],
                               char *const envp[]) -> int __asm__("__wrap_pidfd_spawn");
extern "C" auto RealPidfdSpawn(int *pidfd, const char *path,
                               const posix_spawn_file_actions_t *actions,
                               const posix_spawnattr_t *attributes, char *const argv[],
                               char *const envp[]) -> int __asm__("__real_pidfd_spawn");

// Counts calls, and stands in for a kernel without clone3 when asked.
auto TestPidfdSpawn(int *pidfd, const char *path, const posix_spawn_file_actions_t *actions,
                    const posix_spawnattr_t *attributes, char *const argv[], char *const envp[])
    -> int {
	++SpawnState().calls;
	if (SpawnState().force_enosys) {
		return ENOSYS;
	}
	return RealPidfdSpawn(pidfd, path, actions, attributes, argv, envp);
}

// NOLINTEND(bugprone-easily-swappable-parameters)

extern "C" auto TestPidfdGetpid(int pidfd) -> pid_t __asm__("__wrap_pidfd_getpid");
extern "C" auto RealPidfdGetpid(int pidfd) -> pid_t __asm__("__real_pidfd_getpid");

// Stands in, when asked, for a pidfd_getpid that fails for a reason other than a reaped child:
// everywhere (no readable /proc/self/fdinfo), or only after SpawnChild's probe of this process
// passed.
auto TestPidfdGetpid(int pidfd) -> pid_t {
	const pid_t real  = RealPidfdGetpid(pidfd);
	const auto &state = SpawnState();
	if (state.getpid_error != 0 && (!state.getpid_fails_children_only || real != getpid())) {
		errno = state.getpid_error;
		return -1;
	}
	return real;
}
#endif

namespace {

	using howdy::native::ScopedFd;
	using howdy::pam::runtime::ChildState;
	using howdy::pam::runtime::SpawnChild;
	using howdy::pam::runtime::TrackedChild;
	using howdy::test::Expect;

	constexpr int kCheckedFd = 5;

	struct Spawned {
		int      result = -1;
		pid_t    pid    = -1;
		ScopedFd pidfd;
	};

	// Spawns this test binary in one of its child modes.
	// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
	auto SpawnSelf(std::string_view mode, std::string_view argument,
	               const posix_spawn_file_actions_t *actions = nullptr) -> Spawned {
		std::string           program("child_spawn_test");
		std::string           mode_text(mode);
		std::string           argument_text(argument);
		std::array<char *, 4> argv{program.data(), mode_text.data(), argument_text.data(), nullptr};
		std::array<char *, 1> envp{nullptr};
		Spawned               spawned;
		int                   pidfd = -1;
		spawned.result              = SpawnChild(&spawned.pid, &pidfd,
		                                         {
		                                             .path    = "/proc/self/exe",
		                                             .actions = actions,
		                                             .argv    = argv.data(),
		                                             .envp    = envp.data(),
		                                         });
		spawned.pidfd.Reset(pidfd);
		return spawned;
	}

	auto ExitCode(pid_t pid, int pidfd) -> int {
		TrackedChild child(pid, pidfd);
		int          status = 0;
		if (child.Wait(true, &status) != ChildState::kReaped || !WIFEXITED(status)) {
			return -1;
		}
		return WEXITSTATUS(status);
	}

#ifdef HOWDY_HAVE_PIDFD_SPAWN
	auto CloseOnExec(int fd) -> bool {
		const int flags = fcntl(fd, F_GETFD);
		return flags >= 0 && howdy::native::HasAnyBits(flags, FD_CLOEXEC);
	}
#endif

	auto TestSpawnTakesPidfd() -> bool {
		const int  calls_before = SpawnState().calls;
		const auto spawned      = SpawnSelf("--exit", "7");
		bool ok = Expect(spawned.result == 0 && spawned.pid > 0, "SpawnChild starts the child");
#ifdef HOWDY_HAVE_PIDFD_SPAWN
		ok &= Expect(SpawnState().calls == calls_before + 1 &&
		                 spawned.pidfd.Get() > STDERR_FILENO && CloseOnExec(spawned.pidfd.Get()),
		             "pidfd_spawn returns a close-on-exec pidfd above stderr with the child");
		ok &= Expect(ExitCode(spawned.pid, spawned.pidfd.Get()) == 7,
		             "the child is reaped with its exit status");
#else
		(void)calls_before;
		ok &= Expect(!spawned.pidfd.Valid(), "without pidfd_spawn the caller takes the pidfd");
		int            error = 0;
		const ScopedFd taken(howdy::pam::runtime::TakeSpawnedChildPidfd(spawned.pid, &error));
		ok &= Expect(ExitCode(spawned.pid, taken.Get()) == 7,
		             "the child is reaped with its exit status");
#endif
		return ok;
	}

	auto TestFileActionsHonored() -> bool {
		std::array<int, 2> pipe_fds{-1, -1};
		bool               ok = Expect(pipe2(pipe_fds.data(), O_CLOEXEC) == 0, "create a pipe");
		const ScopedFd     read_end(pipe_fds.at(0));
		const ScopedFd     write_end(pipe_fds.at(1));
		posix_spawn_file_actions_t actions;
		ok &=
		    Expect(posix_spawn_file_actions_init(&actions) == 0 &&
			           posix_spawn_file_actions_adddup2(&actions, write_end.Get(), kCheckedFd) == 0,
			       "build spawn file actions");
		const auto spawned = SpawnSelf("--check-fd", std::to_string(kCheckedFd), &actions);
		(void)posix_spawn_file_actions_destroy(&actions);
		ok &= Expect(spawned.result == 0 && ExitCode(spawned.pid, spawned.pidfd.Get()) == 0,
		             "the child gets the descriptors the file actions place");
		return ok;
	}

	auto TestMissingPath() -> bool {
		std::array<char *, 1> argv{nullptr};
		pid_t                 pid    = -1;
		int                   pidfd  = -1;
		const int             result = SpawnChild(&pid, &pidfd,
		                                          {
		                                              .path    = "/nonexistent/howdy-child",
		                                              .actions = nullptr,
		                                              .argv    = argv.data(),
		                                              .envp    = argv.data(),
		                                          });
		return Expect(result == ENOENT && pid == -1 && pidfd == -1,
		              "a failed spawn reports its error and leaves no child or pidfd");
	}

#ifdef HOWDY_HAVE_PIDFD_SPAWN
	// Without clone3 (ENOSYS), the child comes from posix_spawn and the caller takes the pidfd.
	auto TestEnosysFallsBack() -> bool {
		SpawnState().force_enosys = true;
		const auto spawned        = SpawnSelf("--exit", "3");
		SpawnState().force_enosys = false;
		bool           ok = Expect(spawned.result == 0 && spawned.pid > 0 && !spawned.pidfd.Valid(),
		                           "ENOSYS from pidfd_spawn falls back to posix_spawn");
		int            error = 0;
		const ScopedFd taken(howdy::pam::runtime::TakeSpawnedChildPidfd(spawned.pid, &error));
		ok &= Expect(taken.Valid() && ExitCode(spawned.pid, taken.Get()) == 3,
		             "the fallback child is taken and reaped as before");
		return ok;
	}

	// Where pidfd_getpid does not work (it may need /proc/self/fdinfo), posix_spawn is used as
	// before pidfd_spawn, and the caller takes the pidfd.
	auto TestGetpidUnavailableFallsBack() -> bool {
		const int calls_before    = SpawnState().calls;
		SpawnState().getpid_error = EACCES;
		const auto spawned        = SpawnSelf("--exit", "4");
		SpawnState().getpid_error = 0;
		bool ok    = Expect(spawned.result == 0 && spawned.pid > 0 && !spawned.pidfd.Valid() &&
		                        SpawnState().calls == calls_before,
		                    "where pidfd_getpid does not work, SpawnChild uses posix_spawn");
		int  error = 0;
		const ScopedFd taken(howdy::pam::runtime::TakeSpawnedChildPidfd(spawned.pid, &error));
		ok &= Expect(taken.Valid() && ExitCode(spawned.pid, taken.Get()) == 4,
		             "the posix_spawn child is taken and reaped as before");
		return ok;
	}

	// A pidfd_getpid failure other than ESRCH says nothing about the child, which may still be
	// running: SpawnChild stops and reaps it through the pidfd and fails the spawn.
	auto TestGetpidFailureStopsChild() -> bool {
		SpawnState().getpid_error               = EIO;
		SpawnState().getpid_fails_children_only = true;
		const auto spawned                      = SpawnSelf("--sleep", "30");
		SpawnState().getpid_error               = 0;
		SpawnState().getpid_fails_children_only = false;
		bool      ok = Expect(spawned.result == EIO && spawned.pid == -1 && !spawned.pidfd.Valid(),
		                      "an unexpected pidfd_getpid failure fails the spawn");
		siginfo_t info{};
		ok &= Expect(waitid(P_ALL, 0, &info, WEXITED | WNOHANG) == -1 && errno == ECHILD,
		             "the running child is stopped and reaped before the spawn fails");
		return ok;
	}

	// A PAM application may run with stdio closed; the pidfd still stays clear of it.
	auto TestPidfdAboveClosedStdio() -> bool {
		const pid_t tester = fork();
		if (tester == 0) {
			(void)close(STDIN_FILENO);
			(void)close(STDOUT_FILENO);
			(void)close(STDERR_FILENO);
			const auto spawned = SpawnSelf("--exit", "0");
			const bool above   = spawned.result == 0 && spawned.pidfd.Get() > STDERR_FILENO &&
			                     CloseOnExec(spawned.pidfd.Get());
			_exit(above && ExitCode(spawned.pid, spawned.pidfd.Get()) == 0 ? EXIT_SUCCESS
			                                                               : EXIT_FAILURE);
		}
		int status = 0;
		return Expect(tester > 0 && waitpid(tester, &status, 0) == tester && WIFEXITED(status) &&
		                  WEXITSTATUS(status) == EXIT_SUCCESS,
		              "the pidfd lands above stderr when stdio is closed");
	}
#endif

	// Child modes: exit with a code, sleep for some seconds, or check that a descriptor is open.
	// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
	auto RunChildMode(std::string_view mode, std::string_view argument) -> int {
		if (mode == "--exit") {
			return std::stoi(std::string(argument));
		}
		if (mode == "--sleep") {
			(void)sleep(static_cast<unsigned int>(std::stoi(std::string(argument))));
			return EXIT_SUCCESS;
		}
		if (mode == "--check-fd") {
			return fcntl(std::stoi(std::string(argument)), F_GETFD) >= 0 ? EXIT_SUCCESS
			                                                             : EXIT_FAILURE;
		}
		return EXIT_FAILURE;
	}

}  // namespace

auto main(int argc, char **argv) -> int {
	const auto arguments =
	    std::span(argv, argc > 0 ? static_cast<std::size_t>(argc) : std::size_t{0});
	if (arguments.size() == 3) {
		return RunChildMode(arguments.subspan(1).front(), arguments.back());
	}
	bool ok = true;
	ok &= TestSpawnTakesPidfd();
	ok &= TestFileActionsHonored();
	ok &= TestMissingPath();
#ifdef HOWDY_HAVE_PIDFD_SPAWN
	ok &= TestEnosysFallsBack();
	ok &= TestGetpidUnavailableFallsBack();
	ok &= TestGetpidFailureStopsChild();
	ok &= TestPidfdAboveClosedStdio();
#endif
	return ok ? 0 : 1;
}
