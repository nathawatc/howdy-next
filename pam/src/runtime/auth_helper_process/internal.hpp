#pragma once

#include "runtime/auth_helper_process.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <spawn.h>
#include <string>
#include <string_view>

#include <sys/types.h>

namespace howdy::pam::auth_helper_process::internal {

	using HelperDeadline = std::chrono::steady_clock::time_point;

	constexpr std::size_t kAuthHelperOutputLimit = 9216;
	constexpr auto        kAuthHelperTimeout     = std::chrono::seconds(10);

	struct __attribute__((visibility("hidden"))) PreparedHelperSpawn {
		std::array<int, 2>         output_pipe  = {-1, -1};
		std::array<int, 2>         lease_socket = {-1, -1};
		posix_spawn_file_actions_t actions{};
	};

	enum class HelperWaitResult : std::uint8_t {
		kExited,
		kTimedOut,
		kWaitError,
	};

	enum class HelperReadResult : std::uint8_t {
		kComplete,
		kTimedOut,
		kReadError,
		kOutputLimit,
	};

	enum class LeaseReceiveResult : std::uint8_t {
		kReceived,
		kRetry,
		kInvalid,
	};

	// spawn.cpp
	__attribute__((visibility("hidden"))) auto ProductionOperations() -> Operations;
	__attribute__((visibility("hidden"))) void CloseOwnedFd(const Operations &operations, int &fd);
	__attribute__((visibility("hidden"))) auto SetupHelperSpawn(const Operations    &operations,
	                                                            PreparedHelperSpawn *spawn) -> bool;
	__attribute__((visibility("hidden"))) auto SpawnPrepareHelper(std::string_view     username,
	                                                              const Operations    &operations,
	                                                              PreparedHelperSpawn *spawn,
	                                                              pid_t *child_pid) -> bool;

	// io.cpp
	__attribute__((visibility("hidden"))) auto DeadlinePollTimeout(HelperDeadline deadline) -> int;
	__attribute__((visibility("hidden"))) auto WaitForHelperProcess(pid_t child_pid) -> int;
	__attribute__((visibility("hidden"))) auto TerminateAndReapHelperProcess(pid_t child_pid)
	    -> int;
	__attribute__((visibility("hidden"))) auto
	WaitForHelperProcessUntil(pid_t child_pid, HelperDeadline deadline, int *status)
	    -> HelperWaitResult;
	__attribute__((visibility("hidden"))) auto
	ReadAuthHelperOutputUntil(int output_fd, std::string *output, const Operations &operations,
	                          HelperDeadline deadline) -> HelperReadResult;
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

}  // namespace howdy::pam::auth_helper_process::internal
