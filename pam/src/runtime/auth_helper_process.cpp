#ifndef _GNU_SOURCE
#	define _GNU_SOURCE
#endif

#include "runtime/auth_helper_process.hpp"

#include "auth_helper_process/internal.hpp"
#include "protocol/auth_helper_protocol.hpp"
#include "runtime/runtime_session.hpp"
#include "support/bit_flags.hpp"
#include "support/scoped_fd.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include <stdlib.h>
#include <unistd.h>

#include <sys/poll.h>
#include <sys/syslog.h>
#include <sys/types.h>

namespace howdy::pam::auth_helper_process::internal {

	namespace {
		auto SetRequiredHelperOutputValue(bool *seen, std::string *target, const std::string &value)
		    -> bool {
			if (*seen || value.empty()) {
				return false;
			}
			*seen   = true;
			*target = value;
			return true;
		}
	}  // namespace

	auto ParseAuthHelperOutput(const std::string &output) -> Output {
		Output result;
		bool   saw_config_path     = false;
		bool   saw_user_models_dir = false;

		std::size_t offset = 0;
		while (offset < output.size()) {
			const auto next      = output.find('\n', offset);
			const auto end       = next == std::string::npos ? output.size() : next;
			const auto line      = output.substr(offset, end - offset);
			const auto separator = line.find('=');

			if (separator == std::string::npos) {
				return result;
			}

			const auto key   = line.substr(0, separator);
			const auto value = line.substr(separator + 1);
			if (key == howdy::native::auth_helper_protocol::kConfigPathKey) {
				if (!SetRequiredHelperOutputValue(&saw_config_path, &result.config_path, value)) {
					return result;
				}
			} else if (key == howdy::native::auth_helper_protocol::kUserModelsDirKey) {
				if (!SetRequiredHelperOutputValue(&saw_user_models_dir, &result.user_models_dir,
				                                  value)) {
					return result;
				}
			} else {
				return result;
			}

			if (next == std::string::npos) {
				break;
			}
			offset = next + 1;
		}

		result.valid = saw_config_path && saw_user_models_dir;
		if (!result.valid) {
			return result;
		}

		const std::filesystem::path config_path(result.config_path);
		const std::filesystem::path user_models_dir(result.user_models_dir);
		result.valid = howdy::native::auth_helper_protocol::MatchesPreparedRuntimeLayout(
		    config_path.parent_path(), config_path, user_models_dir, getuid());
		return result;
	}

}  // namespace howdy::pam::auth_helper_process::internal

namespace {

	using howdy::pam::auth_helper_process::Operations;
	using howdy::pam::auth_helper_process::internal::CloseOwnedFd;
	using howdy::pam::auth_helper_process::internal::DeadlinePollTimeout;
	using howdy::pam::auth_helper_process::internal::HelperDeadline;
	using howdy::pam::auth_helper_process::internal::HelperWaitResult;
	using howdy::pam::auth_helper_process::internal::kAuthHelperOutputLimit;
	using howdy::pam::auth_helper_process::internal::LeaseReceiveResult;
	using howdy::pam::auth_helper_process::internal::ParseAuthHelperOutput;
	using howdy::pam::auth_helper_process::internal::PreparedHelperCleanup;
	using howdy::pam::auth_helper_process::internal::PreparedHelperSpawn;

	void CaptureAuthHelperLog(const Operations &operations, std::string_view message) {
		if (operations.log_observer != nullptr) {
			operations.log_observer(operations.context, message);
		}
	}

	void LogPrepareTimeout(const Operations &operations) {
		constexpr std::string_view message = "Howdy auth helper prepare timed out";
		syslog(LOG_AUTHPRIV | LOG_ERR, "pam_howdy: %.*s", static_cast<int>(message.size()),
		       message.data());
		CaptureAuthHelperLog(operations, message);
	}

	enum class PrepareIoResult : std::uint8_t {
		kPending,
		kComplete,
		kFailed,
	};

	auto ReadPrepareOutputOnce(int output_fd, std::string *output) -> PrepareIoResult {
		std::array<char, 4096> buffer{};
		const ssize_t          size = read(output_fd, buffer.data(), buffer.size());
		if (size > 0) {
			if (output->size() + static_cast<std::size_t>(size) >= kAuthHelperOutputLimit) {
				syslog(LOG_AUTHPRIV | LOG_ERR, "pam_howdy: Howdy auth helper reached output limit");
				return PrepareIoResult::kFailed;
			}
			output->append(buffer.data(), static_cast<std::size_t>(size));
			return PrepareIoResult::kPending;
		}
		if (size == 0) {
			return PrepareIoResult::kComplete;
		}
		if (errno == EINTR || errno == EAGAIN) {
			return PrepareIoResult::kPending;
		}
		howdy::pam::auth_helper_process::internal::LogAuthHelperReadError(errno);
		return PrepareIoResult::kFailed;
	}

	auto ReceivePrepareLeaseOnce(int socket_fd, howdy::native::ScopedFd &lease_fd)
	    -> PrepareIoResult {
		int        received_fd = -1;
		const auto result = howdy::pam::auth_helper_process::internal::ReceiveLeaseDescriptorOnce(
		    socket_fd, &received_fd);
		if (result == LeaseReceiveResult::kReceived) {
			lease_fd.Reset(received_fd);
			return PrepareIoResult::kComplete;
		}
		if (result == LeaseReceiveResult::kRetry) {
			return PrepareIoResult::kPending;
		}
		syslog(LOG_AUTHPRIV | LOG_ERR,
		       "pam_howdy: Howdy auth helper returned malformed lease descriptor");
		return PrepareIoResult::kFailed;
	}

	auto PollDescriptorFailed(const pollfd &descriptor) -> bool {
		return descriptor.fd >= 0 &&
		       howdy::native::HasAnyBits(descriptor.revents, POLLERR, POLLNVAL);
	}

	auto CollectPrepareResponse(const Operations &operations, PreparedHelperSpawn *spawn,
	                            HelperDeadline deadline, std::string *output,
	                            howdy::native::ScopedFd &lease_fd) -> bool {
		output->clear();
		bool output_eof = false;

		while (!output_eof || !lease_fd.Valid()) {
			std::array<pollfd, 2> descriptors = {
			    pollfd{.fd      = output_eof ? -1 : spawn->output_pipe.Parent(),
				       .events  = POLLIN,
				       .revents = 0},
			    pollfd{.fd      = lease_fd.Valid() ? -1 : spawn->lease_socket.Parent(),
				       .events  = POLLIN,
				       .revents = 0},
			};
			auto     &output_descriptor = descriptors.front();
			auto     &lease_descriptor  = descriptors.back();
			const int poll_result =
			    poll(descriptors.data(), descriptors.size(), DeadlinePollTimeout(deadline));
			if (poll_result < 0 && errno == EINTR) {
				continue;
			}
			if (poll_result <= 0) {
				LogPrepareTimeout(operations);
				return false;
			}

			if (PollDescriptorFailed(output_descriptor) || PollDescriptorFailed(lease_descriptor)) {
				return false;
			}
			if (output_descriptor.fd >= 0 &&
			    howdy::native::HasAnyBits(output_descriptor.revents, POLLIN, POLLHUP)) {
				const auto result = ReadPrepareOutputOnce(spawn->output_pipe.Parent(), output);
				if (result == PrepareIoResult::kFailed) {
					return false;
				}
				output_eof = result == PrepareIoResult::kComplete;
			}

			if (lease_descriptor.fd >= 0 &&
			    howdy::native::HasAnyBits(lease_descriptor.revents, POLLIN, POLLHUP) &&
			    ReceivePrepareLeaseOnce(spawn->lease_socket.Parent(), lease_fd) ==
			        PrepareIoResult::kFailed) {
				return false;
			}
		}
		return true;
	}

	auto ValidatePrepareHelper(PreparedHelperSpawn &spawn, HelperDeadline deadline,
	                           const std::string &output, const Operations &operations) -> bool {
		int        status = 0;
		const auto result = howdy::pam::auth_helper_process::internal::WaitForHelperProcessUntil(
		    spawn.child_pid, deadline, &status);
		// The nonthrowing wait consumes child ownership on every outcome, including timeout.
		spawn.child_pid = -1;
		if (result != HelperWaitResult::kExited) {
			if (result == HelperWaitResult::kTimedOut) {
				LogPrepareTimeout(operations);
			} else {
				syslog(LOG_AUTHPRIV | LOG_ERR, "pam_howdy: Howdy auth helper failed while waiting");
			}
			return false;
		}
		if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
			syslog(LOG_AUTHPRIV | LOG_ERR, "pam_howdy: Howdy auth helper failed: %s",
			       output.c_str());
			return false;
		}
		return true;
	}

	auto AssignPreparedPaths(const std::string &output, howdy::pam::PreparedRuntimeFiles *prepared)
	    -> bool {
		const auto auth_output = ParseAuthHelperOutput(output);
		if (!auth_output.valid) {
			syslog(LOG_AUTHPRIV | LOG_ERR,
			       "pam_howdy: Howdy auth helper returned malformed output: %s", output.c_str());
			return false;
		}
		prepared->config_path     = auth_output.config_path;
		prepared->user_models_dir = auth_output.user_models_dir;
		prepared->root_dir        = std::filesystem::path(prepared->config_path).parent_path();
		return true;
	}

	auto PrepareRuntimeAuthFilesUntil(std::string_view                  username,
	                                  howdy::pam::PreparedRuntimeFiles *prepared,
	                                  const Operations &operations, HelperDeadline deadline)
	    -> bool {
		PreparedHelperSpawn         spawn;
		const PreparedHelperCleanup cleanup(operations, spawn);
		if (!howdy::pam::auth_helper_process::internal::SetupHelperSpawn(operations, &spawn)) {
			return false;
		}
		if (!howdy::pam::auth_helper_process::internal::SpawnPrepareHelper(username, operations,
		                                                                   &spawn)) {
			return false;
		}
		std::string             helper_output;
		howdy::native::ScopedFd lease_fd;
		const bool              collected =
		    CollectPrepareResponse(operations, &spawn, deadline, &helper_output, lease_fd);
		CloseOwnedFd(operations, spawn.output_pipe.Parent());
		if (!collected) {
			return false;
		}
		if (!ValidatePrepareHelper(spawn, deadline, helper_output, operations)) {
			return false;
		}
		if (!howdy::pam::auth_helper_process::internal::LeaseSocketHasCleanEof(
		        spawn.lease_socket.Parent())) {
			syslog(LOG_AUTHPRIV | LOG_ERR,
			       "pam_howdy: Howdy auth helper returned extra lease data");
			return false;
		}
		CloseOwnedFd(operations, spawn.lease_socket.Parent());
		if (!AssignPreparedPaths(helper_output, prepared) ||
		    !howdy::pam::auth_helper_process::internal::ValidateLeaseDescriptor(
		        lease_fd.Get(), prepared->root_dir, 0)) {
			syslog(LOG_AUTHPRIV | LOG_ERR, "pam_howdy: Howdy auth helper lease validation failed");
			return false;
		}
		prepared->lease_fd = std::move(lease_fd);
		return true;
	}

}  // namespace

namespace howdy::pam::auth_helper_process {

	auto ProductionOperations() -> Operations {
		return internal::ProductionOperations();
	}

	auto PrepareRuntimeAuthFiles(std::string_view username, PreparedRuntimeFiles *prepared,
	                             const Operations &operations) -> bool {
		return PrepareRuntimeAuthFiles(username, prepared, operations,
		                               std::chrono::steady_clock::now() +
		                                   internal::kAuthHelperTimeout);
	}

	auto PrepareRuntimeAuthFiles(std::string_view username, PreparedRuntimeFiles *prepared,
	                             const Operations                     &operations,
	                             std::chrono::steady_clock::time_point deadline) -> bool {
		return ::PrepareRuntimeAuthFilesUntil(username, prepared, operations, deadline);
	}

	auto PrepareRuntimeAuthFiles(std::string_view username, PreparedRuntimeFiles *prepared)
	    -> bool {
		auto operations = ProductionOperations();
		return ::PrepareRuntimeAuthFilesUntil(username, prepared, operations,
		                                      std::chrono::steady_clock::now() +
		                                          internal::kAuthHelperTimeout);
	}

}  // namespace howdy::pam::auth_helper_process
