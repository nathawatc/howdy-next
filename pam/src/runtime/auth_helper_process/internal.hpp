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
	__attribute__((visibility("hidden"))) auto production_operations() -> Operations;
	__attribute__((visibility("hidden"))) void close_owned_fd(const Operations &operations,
	                                                          int              &fd);
	__attribute__((visibility("hidden"))) auto setup_helper_spawn(const Operations    &operations,
	                                                              PreparedHelperSpawn *spawn)
	    -> bool;
	__attribute__((visibility("hidden"))) auto spawn_prepare_helper(std::string_view     username,
	                                                                const Operations    &operations,
	                                                                PreparedHelperSpawn *spawn,
	                                                                pid_t *child_pid) -> bool;

	// io.cpp
	__attribute__((visibility("hidden"))) auto deadline_poll_timeout(HelperDeadline deadline)
	    -> int;
	__attribute__((visibility("hidden"))) auto wait_for_helper_process(pid_t child_pid) -> int;
	__attribute__((visibility("hidden"))) auto terminate_and_reap_helper_process(pid_t child_pid)
	    -> int;
	__attribute__((visibility("hidden"))) auto
	wait_for_helper_process_until(pid_t child_pid, HelperDeadline deadline, int *status)
	    -> HelperWaitResult;
	__attribute__((visibility("hidden"))) auto
	read_auth_helper_output_until(int output_fd, std::string *output, const Operations &operations,
	                              HelperDeadline deadline) -> HelperReadResult;
	__attribute__((visibility("hidden"))) void log_auth_helper_read_error(int error_number);

	// lease.cpp
	__attribute__((visibility("hidden"))) auto receive_lease_descriptor_once(int  socket_fd,
	                                                                         int *lease_fd)
	    -> LeaseReceiveResult;
	__attribute__((visibility("hidden"))) auto
	receive_lease_descriptor_until(int socket_fd, int *lease_fd, HelperDeadline deadline) -> bool;
	__attribute__((visibility("hidden"))) auto lease_socket_has_clean_eof(int socket_fd) -> bool;
	__attribute__((visibility("hidden"))) auto
	validate_lease_descriptor(int lease_fd, const std::filesystem::path &root_dir, uid_t owner_uid)
	    -> bool;

}  // namespace howdy::pam::auth_helper_process::internal
