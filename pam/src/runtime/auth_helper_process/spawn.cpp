#ifndef _GNU_SOURCE
#	define _GNU_SOURCE
#endif

#include "internal.hpp"
#include "protocol/runtime_descriptor_message.hpp"
#include "runtime/auth_helper_process.hpp"
#include "runtime/child_process.hpp"
#include "support/bit_flags.hpp"
#include "support/error_message.hpp"
#include "support/scoped_fd.hpp"

#include <array>
#include <cerrno>
#include <paths.hpp>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/syslog.h>

namespace {

	using AuthHelperOperations   = howdy::pam::auth_helper_process::Operations;
	using AuthHelperSpawnRequest = howdy::pam::auth_helper_process::SpawnRequest;
	using howdy::native::runtime_descriptor_message::kHelperSocketFd;
	using howdy::pam::auth_helper_process::internal::CloseOwnedFd;
	using howdy::pam::auth_helper_process::internal::HelperSpawn;
	using howdy::pam::auth_helper_process::internal::LogAuthHelperFailure;
	using howdy::pam::auth_helper_process::internal::ProcessChannel;

	constexpr auto kNullDevice = "/dev/null";

	// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
	auto ProductionSocketpair(void *context, int domain, int type, int protocol, int *socket_fds)
	    -> int {
		(void)context;
		return socketpair(domain, type, protocol, socket_fds);
	}

	auto ProductionDuplicateFd(void *context, int fd, int minimum_fd) -> int {
		(void)context;
		return fcntl(fd, F_DUPFD_CLOEXEC, minimum_fd);
	}

	auto ProductionActionsInit(void *context, posix_spawn_file_actions_t *actions) -> int {
		(void)context;
		return posix_spawn_file_actions_init(actions);
	}

	auto ProductionActionsAddopen(void *context, posix_spawn_file_actions_t *actions, int fd,
	                              const char *path, int flags) -> int {
		(void)context;
		return posix_spawn_file_actions_addopen(actions, fd, path, flags, 0);
	}

	auto ProductionActionsAdddup2(void *context, posix_spawn_file_actions_t *actions, int source_fd,
	                              int target_fd) -> int {
		(void)context;
		return posix_spawn_file_actions_adddup2(actions, source_fd, target_fd);
	}

	auto ProductionActionsAddclosefrom(void *context, posix_spawn_file_actions_t *actions,
	                                   int from_fd) -> int {
		(void)context;
		return posix_spawn_file_actions_addclosefrom_np(actions, from_fd);
	}

	auto ProductionActionsDestroy(void *context, posix_spawn_file_actions_t *actions) -> int {
		(void)context;
		return posix_spawn_file_actions_destroy(actions);
	}

	auto ProductionSpawn(const AuthHelperSpawnRequest &request) -> int {
		(void)request.context;
		return howdy::pam::runtime::SpawnChild(request.child_pid, request.child_pidfd,
		                                       {
		                                           .path    = request.path,
		                                           .actions = request.actions,
		                                           .argv    = request.argv.data(),
		                                           .envp    = request.envp.data(),
		                                       });
	}

	auto ProductionClose(void *context, int fd) -> int {
		(void)context;
		return close(fd);
	}

	// Moves both ends above the helper's socket descriptor, so the dup2 onto it in the child
	// never meets one of them (a dup2 onto itself would also leave it close-on-exec).
	auto RaiseSocketEnds(const AuthHelperOperations &operations, ProcessChannel &channel) -> bool {
		for (int &end : channel.Descriptors()) {
			if (end > kHelperSocketFd) {
				continue;
			}
			const int raised =
			    operations.duplicate_fd(operations.context, end, kHelperSocketFd + 1);
			if (raised < 0) {
				LogAuthHelperFailure(operations, "fcntl(F_DUPFD_CLOEXEC)", errno);
				CloseOwnedFd(operations, channel.Parent());
				CloseOwnedFd(operations, channel.Child());
				return false;
			}
			CloseOwnedFd(operations, end);
			end = raised;
		}
		return true;
	}

	auto FailSpawnSetup(const AuthHelperOperations &operations, HelperSpawn *spawn,
	                    const char *operation, int error_code) -> bool {
		LogAuthHelperFailure(operations, operation, error_code);
		howdy::pam::auth_helper_process::internal::DestroySpawnActions(operations, *spawn);
		CloseOwnedFd(operations, spawn->socket.Parent());
		CloseOwnedFd(operations, spawn->socket.Child());
		return false;
	}

}  // namespace

namespace howdy::pam::auth_helper_process::internal {

	void LogAuthHelperMessage(const AuthHelperOperations &operations, int priority,
	                          std::string_view message) {
		syslog(LOG_AUTHPRIV | priority, "pam_howdy: %.*s", static_cast<int>(message.size()),
		       message.data());
		if (operations.log_observer != nullptr) {
			operations.log_observer(operations.context, message);
		}
	}

	void LogAuthHelperFailure(const AuthHelperOperations &operations, const char *operation,
	                          int error_code) noexcept {
		try {
			const std::string error_message = howdy::native::SystemErrorMessage(error_code);
			syslog(LOG_AUTHPRIV | LOG_ERR, "pam_howdy: %s failed for auth helper: %s (%d)",
			       operation, error_message.c_str(), error_code);
			if (operations.log_observer != nullptr) {
				const std::string message = std::string(operation) +
				                            " failed for auth helper: " + error_message + " (" +
				                            std::to_string(error_code) + ")";
				operations.log_observer(operations.context, message);
			}
		} catch (...) {
			syslog(LOG_AUTHPRIV | LOG_ERR, "pam_howdy: %s failed for auth helper (%d)", operation,
			       error_code);
		}
	}

	HelperSpawnCleanup::HelperSpawnCleanup(const Operations &operations, HelperSpawn &spawn)
	    : operations_(operations)
	    , spawn_(spawn) {}

	HelperSpawnCleanup::~HelperSpawnCleanup() {
		DestroySpawnActions(operations_, spawn_);
		CloseOwnedFd(operations_, spawn_.socket.Parent());
		CloseOwnedFd(operations_, spawn_.socket.Child());
		if (spawn_.child_pid > 0) {
			(void)TerminateAndReapHelperProcess(spawn_.child_pid, spawn_.child_pidfd.Get());
		}
	}

	void DestroySpawnActions(const Operations &operations, HelperSpawn &spawn) noexcept {
		if (!std::exchange(spawn.actions_initialized, false)) {
			return;
		}
		try {
			const int result = operations.actions_destroy(operations.context, &spawn.actions);
			if (result != 0) {
				LogAuthHelperFailure(operations, "posix_spawn_file_actions_destroy", result);
			}
		} catch (...) {
			syslog(LOG_AUTHPRIV | LOG_ERR,
			       "pam_howdy: Exception destroying auth helper spawn actions");
		}
	}

	auto ProductionOperations() -> AuthHelperOperations {
		return {
		    .socketpair           = ProductionSocketpair,
		    .duplicate_fd         = ProductionDuplicateFd,
		    .actions_init         = ProductionActionsInit,
		    .actions_addopen      = ProductionActionsAddopen,
		    .actions_adddup2      = ProductionActionsAdddup2,
		    .actions_addclosefrom = ProductionActionsAddclosefrom,
		    .actions_destroy      = ProductionActionsDestroy,
		    .spawn                = ProductionSpawn,
		    .close                = ProductionClose,
		};
	}

	void CloseOwnedFd(const AuthHelperOperations &operations, int &fd) noexcept {
		if (fd < 0) {
			return;
		}
		const int owned_fd = std::exchange(fd, -1);
		try {
			(void)operations.close(operations.context, owned_fd);
		} catch (...) {
			// Never retry a close: the callback may have closed the descriptor before throwing.
			syslog(LOG_AUTHPRIV | LOG_ERR, "pam_howdy: Exception closing auth helper descriptor");
		}
	}

	// The helper gets /dev/null as stdio (its diagnostics go to syslog), its end of the socket at
	// kHelperSocketFd, and nothing else.
	auto SetupHelperSpawn(const AuthHelperOperations &operations, HelperSpawn *spawn) -> bool {
		if (operations.socketpair(operations.context, AF_UNIX,
		                          howdy::native::CombineBitsAs<int>(SOCK_SEQPACKET, SOCK_CLOEXEC),
		                          0, spawn->socket.Data()) != 0) {
			spawn->socket = ProcessChannel{};
			LogAuthHelperFailure(operations, "socketpair", errno);
			return false;
		}
		if (!RaiseSocketEnds(operations, spawn->socket)) {
			return false;
		}
		const int init_result = operations.actions_init(operations.context, &spawn->actions);
		if (init_result != 0) {
			LogAuthHelperFailure(operations, "posix_spawn_file_actions_init", init_result);
			CloseOwnedFd(operations, spawn->socket.Parent());
			CloseOwnedFd(operations, spawn->socket.Child());
			return false;
		}
		spawn->actions_initialized = true;

		struct StdioTarget {
			int         fd;
			int         flags;
			const char *operation;
		};

		for (const auto &target : {
		         StdioTarget{
		             .fd        = STDIN_FILENO,
		             .flags     = O_RDONLY,
		             .operation = "posix_spawn_file_actions_addopen(stdin)",
		         },
		         StdioTarget{
		             .fd        = STDOUT_FILENO,
		             .flags     = O_WRONLY,
		             .operation = "posix_spawn_file_actions_addopen(stdout)",
		         },
		         StdioTarget{
		             .fd        = STDERR_FILENO,
		             .flags     = O_WRONLY,
		             .operation = "posix_spawn_file_actions_addopen(stderr)",
		         },
		     }) {
			const int result = operations.actions_addopen(operations.context, &spawn->actions,
			                                              target.fd, kNullDevice, target.flags);
			if (result != 0) {
				return FailSpawnSetup(operations, spawn, target.operation, result);
			}
		}
		int result = operations.actions_adddup2(operations.context, &spawn->actions,
		                                        spawn->socket.Child(), kHelperSocketFd);
		if (result != 0) {
			return FailSpawnSetup(operations, spawn, "posix_spawn_file_actions_adddup2(socket)",
			                      result);
		}
		// Both socket ends sit above kHelperSocketFd, so this closes them too.
		result = operations.actions_addclosefrom(operations.context, &spawn->actions,
		                                         kHelperSocketFd + 1);
		if (result != 0) {
			return FailSpawnSetup(operations, spawn, "posix_spawn_file_actions_addclosefrom",
			                      result);
		}
		return true;
	}

	auto SpawnOpenHelper(std::string_view username, const AuthHelperOperations &operations,
	                     HelperSpawn *spawn) -> bool {
		std::string           helper_path(kAuthHelperPath);
		std::string           command("open");
		std::string           username_string(username);
		std::array<char *, 4> args = {
		    helper_path.data(),
		    command.data(),
		    username_string.data(),
		    nullptr,
		};
		std::array<char *, 1> env         = {nullptr};
		int                   spawn_pidfd = -1;
		const int             result      = operations.spawn({
		    .context     = operations.context,
		    .child_pid   = &spawn->child_pid,
		    .child_pidfd = &spawn_pidfd,
		    .path        = kAuthHelperPath,
		    .actions     = &spawn->actions,
		    .argv        = args,
		    .envp        = env,
		});
		// Owned at once, so it is closed on every path below that does not keep it.
		howdy::native::ScopedFd taken_pidfd(spawn_pidfd);
		bool                    spawned = result == 0;
		if (!spawned) {
			spawn->child_pid = -1;
			// SpawnChild's error may come from pidfd_spawn, pidfd_getpid or posix_spawn.
			LogAuthHelperFailure(operations, "spawn", result);
		} else if (taken_pidfd.Valid()) {
			// Taken with the child, so it names the helper whatever the application reaps.
			if (spawn->child_pid > 0) {
				spawn->child_pidfd = std::move(taken_pidfd);
			} else {
				// Already exited and reaped by the application: nothing is left to wait for or
				// stop, and its message, already in the socket, decides.
				spawn->child_pid = -1;
				LogAuthHelperMessage(
				    operations, LOG_DEBUG,
				    "Howdy auth helper was reaped elsewhere before its PID was read");
			}
		} else if (spawn->child_pid <= 0) {
			spawned          = false;
			spawn->child_pid = -1;
			LogAuthHelperFailure(operations, "posix_spawn", ECHILD);
		} else {
			// A pidfd opened later could name another process, so a failure fails the open,
			// unless the helper is already gone: then nothing is left to name or stop, and its
			// message, already in the socket, decides.
			int error_number = 0;
			spawn->child_pidfd.Reset(
			    howdy::pam::runtime::TakeSpawnedChildPidfd(spawn->child_pid, &error_number));
			if (!spawn->child_pidfd.Valid() &&
			    howdy::pam::runtime::PidfdErrorMeansChildGone(error_number)) {
				spawn->child_pid = -1;
				LogAuthHelperMessage(
				    operations, LOG_DEBUG,
				    "Howdy auth helper was reaped elsewhere before its pidfd was taken");
			} else if (!spawn->child_pidfd.Valid()) {
				LogAuthHelperFailure(operations, "pidfd_open", error_number);
				spawn->child_pid = -1;
				spawned          = false;
			}
		}
		DestroySpawnActions(operations, *spawn);
		CloseOwnedFd(operations, spawn->socket.Child());
		if (!spawned) {
			CloseOwnedFd(operations, spawn->socket.Parent());
			return false;
		}
		return true;
	}

}  // namespace howdy::pam::auth_helper_process::internal
