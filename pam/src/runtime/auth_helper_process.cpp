#ifndef _GNU_SOURCE
#	define _GNU_SOURCE
#endif

#include "runtime/auth_helper_process.hpp"

#include "auth_helper_process/internal.hpp"
#include "protocol/auth_helper_protocol.hpp"
#include "runtime/runtime_session.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <poll.h>
#include <string>
#include <string_view>
#include <syslog.h>
#include <unistd.h>

#include <sys/wait.h>

namespace {

	using AuthHelperOutput = howdy::pam::auth_helper_process::Output;
	using howdy::pam::auth_helper_process::Operations;
	using howdy::pam::auth_helper_process::internal::CloseOwnedFd;
	using howdy::pam::auth_helper_process::internal::DeadlinePollTimeout;
	using howdy::pam::auth_helper_process::internal::HelperDeadline;
	using howdy::pam::auth_helper_process::internal::HelperWaitResult;
	using howdy::pam::auth_helper_process::internal::kAuthHelperOutputLimit;
	using howdy::pam::auth_helper_process::internal::kAuthHelperTimeout;
	using howdy::pam::auth_helper_process::internal::LeaseReceiveResult;
	using howdy::pam::auth_helper_process::internal::PreparedHelperSpawn;

	auto SetRequiredHelperOutputValue(bool *seen, std::string *target, const std::string &value)
	    -> bool {
		if (*seen || value.empty()) {
			return false;
		}
		*seen   = true;
		*target = value;
		return true;
	}

	auto ParseAuthHelperOutput(const std::string &output) -> AuthHelperOutput {
		AuthHelperOutput result;
		bool             saw_config_path     = false;
		bool             saw_user_models_dir = false;

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

	void CaptureAuthHelperLog(const Operations &operations, std::string_view message) {
		if (operations.log_observer != nullptr) {
			operations.log_observer(operations.context, message);
		}
	}

	void LogPrepareTimeout(const Operations &operations) {
		constexpr std::string_view message = "Howdy auth helper prepare timed out";
		syslog(LOG_ERR, "%.*s", static_cast<int>(message.size()), message.data());
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
				syslog(LOG_ERR, "Howdy auth helper reached output limit");
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

	auto ReceivePrepareLeaseOnce(int socket_fd, int *lease_fd) -> PrepareIoResult {
		const auto result = howdy::pam::auth_helper_process::internal::ReceiveLeaseDescriptorOnce(
		    socket_fd, lease_fd);
		if (result == LeaseReceiveResult::kReceived) {
			return PrepareIoResult::kComplete;
		}
		if (result == LeaseReceiveResult::kRetry) {
			return PrepareIoResult::kPending;
		}
		syslog(LOG_ERR, "Howdy auth helper returned malformed lease descriptor");
		return PrepareIoResult::kFailed;
	}

	auto PollDescriptorFailed(const pollfd &descriptor) -> bool {
		return descriptor.fd >= 0 && (descriptor.revents & (POLLERR | POLLNVAL)) != 0;
	}

	auto CollectPrepareResponse(const Operations &operations, PreparedHelperSpawn *spawn,
	                            HelperDeadline deadline, std::string *output, int *lease_fd)
	    -> bool {
		output->clear();
		*lease_fd       = -1;
		bool output_eof = false;

		while (!output_eof || *lease_fd < 0) {
			std::array<pollfd, 2> descriptors = {
			    pollfd{
			        .fd = output_eof ? -1 : spawn->output_pipe[0], .events = POLLIN, .revents = 0},
			    pollfd{.fd      = *lease_fd >= 0 ? -1 : spawn->lease_socket[0],
			           .events  = POLLIN,
			           .revents = 0},
			};
			const int poll_result =
			    poll(descriptors.data(), descriptors.size(), DeadlinePollTimeout(deadline));
			if (poll_result < 0 && errno == EINTR) {
				continue;
			}
			if (poll_result <= 0) {
				LogPrepareTimeout(operations);
				return false;
			}

			if (PollDescriptorFailed(descriptors[0]) || PollDescriptorFailed(descriptors[1])) {
				return false;
			}
			if (descriptors[0].fd >= 0 && (descriptors[0].revents & (POLLIN | POLLHUP)) != 0) {
				const auto result = ReadPrepareOutputOnce(spawn->output_pipe[0], output);
				if (result == PrepareIoResult::kFailed) {
					return false;
				}
				output_eof = result == PrepareIoResult::kComplete;
			}

			if (descriptors[1].fd >= 0 && (descriptors[1].revents & (POLLIN | POLLHUP)) != 0 &&
			    ReceivePrepareLeaseOnce(spawn->lease_socket[0], lease_fd) ==
			        PrepareIoResult::kFailed) {
				return false;
			}
		}
		return true;
	}

	auto ValidatePrepareHelper(pid_t child_pid, HelperDeadline deadline, const std::string &output,
	                           const Operations &operations) -> bool {
		int        status = 0;
		const auto result = howdy::pam::auth_helper_process::internal::WaitForHelperProcessUntil(
		    child_pid, deadline, &status);
		if (result != HelperWaitResult::kExited) {
			if (result == HelperWaitResult::kTimedOut) {
				LogPrepareTimeout(operations);
			} else {
				syslog(LOG_ERR, "Howdy auth helper failed while waiting");
			}
			return false;
		}
		if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
			syslog(LOG_ERR, "Howdy auth helper failed: %s", output.c_str());
			return false;
		}
		return true;
	}

	auto AssignPreparedPaths(const std::string &output, howdy::pam::PreparedRuntimeFiles *prepared)
	    -> bool {
		const auto auth_output = ParseAuthHelperOutput(output);
		if (!auth_output.valid) {
			syslog(LOG_ERR, "Howdy auth helper returned malformed output: %s", output.c_str());
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
		PreparedHelperSpawn spawn;
		if (!howdy::pam::auth_helper_process::internal::SetupHelperSpawn(operations, &spawn)) {
			return false;
		}
		pid_t child_pid = -1;
		if (!howdy::pam::auth_helper_process::internal::SpawnPrepareHelper(username, operations,
		                                                                   &spawn, &child_pid)) {
			return false;
		}
		std::string helper_output;
		int         lease_fd = -1;
		const bool  collected =
		    CollectPrepareResponse(operations, &spawn, deadline, &helper_output, &lease_fd);
		CloseOwnedFd(operations, spawn.output_pipe[0]);
		if (!collected) {
			CloseOwnedFd(operations, spawn.lease_socket[0]);
			CloseOwnedFd(operations, lease_fd);
			(void)howdy::pam::auth_helper_process::internal::TerminateAndReapHelperProcess(
			    child_pid);
			return false;
		}
		if (!ValidatePrepareHelper(child_pid, deadline, helper_output, operations)) {
			CloseOwnedFd(operations, spawn.lease_socket[0]);
			CloseOwnedFd(operations, lease_fd);
			return false;
		}
		if (!howdy::pam::auth_helper_process::internal::LeaseSocketHasCleanEof(
		        spawn.lease_socket[0])) {
			syslog(LOG_ERR, "Howdy auth helper returned extra lease data");
			CloseOwnedFd(operations, spawn.lease_socket[0]);
			CloseOwnedFd(operations, lease_fd);
			return false;
		}
		CloseOwnedFd(operations, spawn.lease_socket[0]);
		if (!AssignPreparedPaths(helper_output, prepared) ||
		    !howdy::pam::auth_helper_process::internal::ValidateLeaseDescriptor(
		        lease_fd, prepared->root_dir, 0)) {
			syslog(LOG_ERR, "Howdy auth helper lease validation failed");
			CloseOwnedFd(operations, lease_fd);
			return false;
		}
		prepared->lease_fd = lease_fd;
		return true;
	}

}  // namespace

namespace howdy::pam::auth_helper_process {

	auto ProductionOperations() -> Operations {
		return internal::ProductionOperations();
	}

	auto OutputLimit() -> std::size_t {
		return internal::kAuthHelperOutputLimit;
	}

	auto ReceiveLeaseDescriptor(int socket_fd, int *lease_fd,
	                            std::chrono::steady_clock::time_point deadline) -> bool {
		return internal::ReceiveLeaseDescriptorUntil(socket_fd, lease_fd, deadline);
	}

	auto ValidateLeaseDescriptor(int lease_fd, const std::filesystem::path &root_dir,
	                             uid_t owner_uid) -> bool {
		return internal::ValidateLeaseDescriptor(lease_fd, root_dir, owner_uid);
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

	auto ReadOutput(Process process, std::string *output, const Operations &operations,
	                std::chrono::steady_clock::time_point deadline) -> bool {
		const auto read_result =
		    internal::ReadAuthHelperOutputUntil(process.output_fd, output, operations, deadline);
		if (read_result != internal::HelperReadResult::kComplete) {
			if (read_result == internal::HelperReadResult::kTimedOut) {
				LogPrepareTimeout(operations);
			}
			(void)internal::TerminateAndReapHelperProcess(process.child_pid);
			return false;
		}

		int        status = 0;
		const auto wait_result =
		    internal::WaitForHelperProcessUntil(process.child_pid, deadline, &status);
		if (wait_result != internal::HelperWaitResult::kExited) {
			output->clear();
			if (wait_result == internal::HelperWaitResult::kTimedOut) {
				LogPrepareTimeout(operations);
			}
			return false;
		}
		if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS ||
		    !::ParseAuthHelperOutput(*output).valid) {
			output->clear();
			return false;
		}
		return true;
	}

	auto ParseOutput(const std::string &output) -> Output {
		return ::ParseAuthHelperOutput(output);
	}

	auto WaitForHelper(pid_t child_pid) -> int {
		return internal::WaitForHelperProcess(child_pid);
	}

	auto PrepareRuntimeAuthFiles(std::string_view username, PreparedRuntimeFiles *prepared)
	    -> bool {
		auto operations         = ProductionOperations();
		operations.read_bounded = nullptr;
		return ::PrepareRuntimeAuthFilesUntil(username, prepared, operations,
		                                      std::chrono::steady_clock::now() +
		                                          internal::kAuthHelperTimeout);
	}

}  // namespace howdy::pam::auth_helper_process
