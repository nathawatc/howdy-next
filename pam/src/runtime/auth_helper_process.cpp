#ifndef _GNU_SOURCE
#	define _GNU_SOURCE
#endif

#include "runtime/auth_helper_process.hpp"

#include "auth_helper_process/internal.hpp"
#include "protocol/runtime_descriptor_message.hpp"
#include "runtime/runtime_session.hpp"
#include "support/bit_flags.hpp"

#include <cerrno>
#include <chrono>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include <stdlib.h>

#include <sys/poll.h>
#include <sys/syslog.h>

namespace {

	using howdy::pam::auth_helper_process::Operations;
	using howdy::pam::auth_helper_process::internal::CloseOwnedFd;
	using howdy::pam::auth_helper_process::internal::DeadlinePollTimeout;
	using howdy::pam::auth_helper_process::internal::HelperDeadline;
	using howdy::pam::auth_helper_process::internal::HelperSpawn;
	using howdy::pam::auth_helper_process::internal::HelperSpawnCleanup;
	using howdy::pam::auth_helper_process::internal::HelperWaitResult;
	using howdy::pam::auth_helper_process::internal::LogAuthHelperFailure;
	using howdy::pam::auth_helper_process::internal::LogAuthHelperMessage;

	constexpr auto kAuthHelperTimeout = std::chrono::seconds(10);

	// Polls the helper's socket until its one message and then end of file arrive.
	auto ReceiveHelperResult(int socket_fd, HelperDeadline deadline, const Operations &operations,
	                         howdy::pam::RuntimeDescriptorSet *descriptors) -> bool {
		namespace message = howdy::native::runtime_descriptor_message;
		howdy::pam::RuntimeDescriptorSet received;
		bool                             have_message = false;
		while (true) {
			pollfd    socket{.fd = socket_fd, .events = POLLIN, .revents = 0};
			const int ready = poll(&socket, 1, DeadlinePollTimeout(deadline));
			if (ready < 0) {
				if (errno == EINTR) {
					continue;
				}
				LogAuthHelperFailure(operations, "poll", errno);
				return false;
			}
			if (ready == 0) {
				LogAuthHelperMessage(operations, LOG_ERR, "Howdy auth helper open timed out");
				return false;
			}
			if (howdy::native::HasAnyBits(socket.revents, POLLERR, POLLNVAL)) {
				LogAuthHelperMessage(operations, LOG_ERR, "Howdy auth helper socket failed");
				return false;
			}

			auto incoming = message::ReceiveMessage(socket_fd);
			switch (incoming.status) {
				case message::ReceiveStatus::kRetry:
					continue;
				case message::ReceiveStatus::kMessage:
					if (have_message) {
						LogAuthHelperMessage(operations, LOG_ERR,
						                     "Howdy auth helper sent more than one message");
						return false;
					}
					have_message         = true;
					received.config      = std::move(incoming.config);
					received.model_state = incoming.model_state;
					received.model       = std::move(incoming.model);
					continue;
				case message::ReceiveStatus::kEndOfFile:
					if (!have_message) {
						LogAuthHelperMessage(operations, LOG_ERR,
						                     "Howdy auth helper ended without sending runtime "
						                     "descriptors");
						return false;
					}
					*descriptors = std::move(received);
					return true;
				case message::ReceiveStatus::kInvalid:
					LogAuthHelperMessage(operations, LOG_ERR,
					                     "Howdy auth helper sent a malformed message");
					return false;
				case message::ReceiveStatus::kError:
					LogAuthHelperFailure(operations, "recvmsg", incoming.error_number);
					return false;
			}
		}
	}

	auto DescribeWaitStatus(int status) -> std::string {
		if (WIFEXITED(status)) {
			return std::format("exit status {}", WEXITSTATUS(status));
		}
		if (WIFSIGNALED(status)) {
			return std::format("signal {}", WTERMSIG(status));
		}
		return std::format("wait status {}", status);
	}

	// The result is already decided by the message; the exit status is only reaped and logged.
	void ReapFinishedHelper(HelperSpawn &spawn, HelperDeadline deadline,
	                        const Operations &operations) {
		if (spawn.child_pid <= 0) {
			return;
		}
		int        status = 0;
		const auto result = howdy::pam::auth_helper_process::internal::WaitForHelperProcessUntil(
		    spawn.child_pid, deadline, &status, spawn.child_pidfd.Get());
		// The nonthrowing wait consumes child ownership on every outcome, including timeout.
		spawn.child_pid = -1;
		spawn.child_pidfd.Reset();
		switch (result) {
			case HelperWaitResult::kExited:
				if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
					LogAuthHelperMessage(operations, LOG_WARNING,
					                     "Howdy auth helper exited abnormally after its result: " +
					                         DescribeWaitStatus(status));
				}
				return;
			case HelperWaitResult::kReapedElsewhere:
				// Normal on hosts that reap their children, so not worth a line every time.
				LogAuthHelperMessage(operations, LOG_DEBUG,
				                     "Howdy auth helper was reaped elsewhere after its result");
				return;
			case HelperWaitResult::kTimedOut:
				LogAuthHelperMessage(operations, LOG_WARNING,
				                     "Howdy auth helper had to be stopped after its result");
				return;
			case HelperWaitResult::kWaitError:
				LogAuthHelperMessage(operations, LOG_WARNING,
				                     "Howdy auth helper could not be waited for after its result");
				return;
		}
	}

	auto OpenRuntimeDescriptorsUntil(std::string_view                  username,
	                                 howdy::pam::RuntimeDescriptorSet *descriptors,
	                                 const Operations &operations, HelperDeadline deadline)
	    -> bool {
		HelperSpawn              spawn;
		const HelperSpawnCleanup cleanup(operations, spawn);
		if (!howdy::pam::auth_helper_process::internal::SetupHelperSpawn(operations, &spawn) ||
		    !howdy::pam::auth_helper_process::internal::SpawnOpenHelper(username, operations,
		                                                                &spawn)) {
			return false;
		}
		howdy::pam::RuntimeDescriptorSet received;
		const bool ok = ReceiveHelperResult(spawn.socket.Parent(), deadline, operations, &received);
		CloseOwnedFd(operations, spawn.socket.Parent());
		if (!ok) {
			return false;  // the cleanup stops and reaps a helper that is still ours
		}
		ReapFinishedHelper(spawn, deadline, operations);
		*descriptors = std::move(received);
		return true;
	}

}  // namespace

namespace howdy::pam::auth_helper_process {

	auto ProductionOperations() -> Operations {
		return internal::ProductionOperations();
	}

	auto OpenRuntimeDescriptors(std::string_view username, RuntimeDescriptorSet *descriptors,
	                            const Operations                     &operations,
	                            std::chrono::steady_clock::time_point deadline) -> bool {
		if (username.empty() || descriptors == nullptr || operations.socketpair == nullptr ||
		    operations.duplicate_fd == nullptr || operations.actions_init == nullptr ||
		    operations.actions_addopen == nullptr || operations.actions_adddup2 == nullptr ||
		    operations.actions_addclosefrom == nullptr || operations.actions_destroy == nullptr ||
		    operations.spawn == nullptr || operations.close == nullptr) {
			LogAuthHelperMessage(operations, LOG_ERR,
			                     "Howdy auth helper open request is incomplete");
			return false;
		}
		return ::OpenRuntimeDescriptorsUntil(username, descriptors, operations, deadline);
	}

	auto OpenRuntimeDescriptors(std::string_view username, RuntimeDescriptorSet *descriptors,
	                            const Operations &operations) -> bool {
		return OpenRuntimeDescriptors(username, descriptors, operations,
		                              std::chrono::steady_clock::now() + kAuthHelperTimeout);
	}

	auto OpenRuntimeDescriptors(std::string_view username, RuntimeDescriptorSet *descriptors)
	    -> bool {
		const auto operations = ProductionOperations();
		return OpenRuntimeDescriptors(username, descriptors, operations);
	}

}  // namespace howdy::pam::auth_helper_process
