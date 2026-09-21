#pragma once

#include "runtime/auth_helper_process.hpp"

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
		kWaitError,
	};

	enum class LeaseReceiveResult : std::uint8_t {
		kReceived,
		kRetry,
		kInvalid,
	};

	// spawn.cpp
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
	__attribute__((visibility("hidden"))) auto WaitForHelperProcess(pid_t child_pid) noexcept
	    -> int;
	__attribute__((visibility("hidden"))) auto
	TerminateAndReapHelperProcess(pid_t child_pid) noexcept -> int;
	__attribute__((visibility("hidden"))) auto
	WaitForHelperProcessUntil(pid_t child_pid, HelperDeadline deadline, int *status) noexcept
	    -> HelperWaitResult;
	__attribute__((visibility("hidden"))) void LogAuthHelperReadError(int error_number);

	// lease.cpp
	__attribute__((visibility("hidden"))) auto ReceiveLeaseDescriptorOnce(int  socket_fd,
	                                                                      int *lease_fd)
	    -> LeaseReceiveResult;
	__attribute__((visibility("hidden"))) auto
	ReceiveLeaseDescriptorUntil(int socket_fd, int *lease_fd, HelperDeadline deadline) -> bool;
	__attribute__((visibility("hidden"))) auto LeaseSocketHasCleanEof(int socket_fd) -> bool;
	__attribute__((visibility("hidden"))) auto
	ValidateLeaseDescriptor(int lease_fd, const std::filesystem::path &root_dir, uid_t owner_uid)
	    -> bool;

	// auth_helper_process.cpp
	__attribute__((visibility("hidden"))) auto ParseAuthHelperOutput(const std::string &output)
	    -> Output;

}  // namespace howdy::pam::auth_helper_process::internal
