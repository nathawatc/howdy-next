#pragma once

#include "runtime/auth_helper_process.hpp"
#include "support/scoped_fd.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include <spawn.h>

#include <sys/types.h>

namespace howdy::pam::auth_helper_process::internal {

	using HelperDeadline = std::chrono::steady_clock::time_point;

	constexpr std::size_t kAuthHelperOutputLimit = 9216;
	constexpr auto        kAuthHelperTimeout     = std::chrono::seconds(10);

	class __attribute__((visibility("hidden"))) ProcessChannel {
	public:
		[[nodiscard]] auto Data() noexcept -> int * {
			return descriptors_.data();
		}

		[[nodiscard]] auto Descriptors() noexcept -> std::span<int> {
			return descriptors_;
		}

		[[nodiscard]] auto Parent() noexcept -> int & {
			return descriptors_.front();
		}

		[[nodiscard]] auto Child() noexcept -> int & {
			return descriptors_.back();
		}

	private:
		std::array<int, 2> descriptors_ = {-1, -1};
	};

	struct __attribute__((visibility("hidden"))) PreparedHelperSpawn {
		ProcessChannel             output_pipe;
		ProcessChannel             lease_socket;
		posix_spawn_file_actions_t actions{};
		bool                       actions_initialized = false;
		pid_t                      child_pid           = -1;
		// Taken right after spawn, so later waits and signals name the helper itself even if
		// the PAM application reaps it and the PID is reused during the helper's I/O.
		howdy::native::ScopedFd child_pidfd;
		// The helper had already exited and been reaped by the application when the pidfd was
		// taken (runtime::PidfdErrorMeansChildGone): its output and lease are in the channels,
		// and its completion record decides, as for kReapedElsewhere.
		bool reaped_before_pidfd = false;
	};

	class __attribute__((visibility("hidden"))) PreparedHelperCleanup {
	public:
		PreparedHelperCleanup(const Operations &operations, PreparedHelperSpawn &spawn);
		~PreparedHelperCleanup();
		PreparedHelperCleanup(const PreparedHelperCleanup &)                     = delete;
		auto operator=(const PreparedHelperCleanup &) -> PreparedHelperCleanup & = delete;
		PreparedHelperCleanup(PreparedHelperCleanup &&)                          = delete;
		auto operator=(PreparedHelperCleanup &&) -> PreparedHelperCleanup &      = delete;

	private:
		const Operations    &operations_;
		PreparedHelperSpawn &spawn_;
	};

	enum class HelperWaitResult : std::uint8_t {
		kExited,
		kTimedOut,
		// ECHILD: the helper is no longer a child this process can wait for, normally because the
		// PAM application reaped it and took its exit status.
		kReapedElsewhere,
		kWaitError,
	};

	enum class LeaseReceiveResult : std::uint8_t {
		kReceived,
		kRetry,
		kInvalid,
	};

	// spawn.cpp
	// Logs `message` to syslog at `priority` and to the log observer. An exception from the
	// observer propagates, as the prepare's cleanup tests rely on.
	__attribute__((visibility("hidden"))) void
	LogAuthHelperMessage(const Operations &operations, int priority, std::string_view message);
	// Logs "<operation> failed for auth helper: <error text> (<error_code>)" to syslog and the
	// log observer, falling back to a plain syslog line if building the message fails.
	__attribute__((visibility("hidden"))) void LogAuthHelperFailure(const Operations &operations,
	                                                                const char       *operation,
	                                                                int error_code) noexcept;
	__attribute__((visibility("hidden"))) void
	DestroySpawnActions(const Operations &operations, PreparedHelperSpawn &spawn) noexcept;
	__attribute__((visibility("hidden"))) auto ProductionOperations() -> Operations;
	__attribute__((visibility("hidden"))) void CloseOwnedFd(const Operations &operations,
	                                                        int              &fd) noexcept;
	__attribute__((visibility("hidden"))) auto SetupHelperSpawn(const Operations    &operations,
	                                                            PreparedHelperSpawn *spawn) -> bool;
	__attribute__((visibility("hidden"))) auto SpawnPrepareHelper(std::string_view     username,
	                                                              const Operations    &operations,
	                                                              PreparedHelperSpawn *spawn)
	    -> bool;

	// io.cpp
	__attribute__((visibility("hidden"))) auto DeadlinePollTimeout(HelperDeadline deadline) -> int;
	// `pidfd` is the borrowed descriptor taken at spawn and must be passed explicitly. With -1
	// every operation uses the PID, each only after the kernel reports an unreaped child with it;
	// production always passes one, and only tests use -1.
	__attribute__((visibility("hidden"))) auto TerminateAndReapHelperProcess(pid_t child_pid,
	                                                                         int   pidfd) noexcept
	    -> int;
	__attribute__((visibility("hidden"))) auto
	WaitForHelperProcessUntil(pid_t child_pid, HelperDeadline deadline, int *status,
	                          int pidfd) noexcept -> HelperWaitResult;
	// The result for a helper reaped elsewhere: past the deadline it counts as a timeout, as a
	// helper the wait collects late does; otherwise its completion record decides.
	__attribute__((visibility("hidden"))) auto
	ReapedElsewhereResult(HelperDeadline deadline) noexcept -> HelperWaitResult;
	__attribute__((visibility("hidden"))) void LogAuthHelperReadError(int error_number);

	// lease.cpp
	__attribute__((visibility("hidden"))) auto ReceiveLeaseDescriptorOnce(int  socket_fd,
	                                                                      int *lease_fd)
	    -> LeaseReceiveResult;
	__attribute__((visibility("hidden"))) auto LeaseSocketHasCleanEof(int socket_fd) -> bool;

	enum class LeaseSocketEnd : std::uint8_t {
		kCleanEof,
		// The completion record (protocol/auth_helper_protocol.hpp), then a clean end of file.
		kCompletedThenEof,
		kInvalid,
	};

	// What follows the lease once the helper is gone: nothing, or only the completion record.
	__attribute__((visibility("hidden"))) auto ReceiveLeaseSocketEnd(int socket_fd)
	    -> LeaseSocketEnd;
	__attribute__((visibility("hidden"))) auto
	ValidateLeaseDescriptor(int lease_fd, const std::filesystem::path &root_dir, uid_t owner_uid)
	    -> bool;

	// auth_helper_process.cpp
	__attribute__((visibility("hidden"))) auto
	ParseAuthHelperOutput(const std::string &output, const std::filesystem::path &runtime_root)
	    -> Output;

}  // namespace howdy::pam::auth_helper_process::internal
