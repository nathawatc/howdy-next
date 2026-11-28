#include "auth_helper/command.hpp"

#include "auth_helper/runtime_descriptors.hpp"
#include "config/runtime_paths.hpp"
#include "protocol/runtime_descriptor_message.hpp"
#include "storage/user_model_store.hpp"
#include "support/passwd_lookup.hpp"
#include "support/scoped_stream_capture.hpp"
#include "support/user_names.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include <unistd.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syslog.h>

namespace howdy::native::auth_helper::command {
	namespace {
		auto Usage(const char *argv0) -> void {
			std::cout << "Usage: " << argv0 << " open <user>\n";
		}

		auto Fail(std::string_view message) -> int {
			std::cerr << message << "\n";
			return 1;
		}

		// PAM points `open`'s stdio at /dev/null, so its failures also go to syslog.
		auto FailAndLog(std::string_view message) -> int {
			SyslogDiagnosticLines(message);
			return Fail(message);
		}

		constexpr std::string_view kExceptionPrefix = "howdy-auth-helper failed: ";
		constexpr std::string_view kUnknownExceptionMessage =
		    "howdy-auth-helper failed: unknown error";
		constexpr std::size_t kExceptionBufferSize = 512;

		// Formats into a caller-owned stack buffer so OOM (std::bad_alloc) never allocates while
		// reporting the failure.
		auto FormatExceptionMessage(std::string_view detail, std::span<char> buffer) noexcept
		    -> std::string_view {
			if (detail.empty()) {
				return kUnknownExceptionMessage;
			}
			const std::size_t prefix_len = std::min(kExceptionPrefix.size(), buffer.size());
			std::ranges::copy(kExceptionPrefix.substr(0, prefix_len), buffer.begin());
			const std::size_t detail_len = std::min(detail.size(), buffer.size() - prefix_len);
			std::ranges::copy(detail.substr(0, detail_len), buffer.subspan(prefix_len).begin());
			return {buffer.data(), prefix_len + detail_len};
		}

		template <typename LogFn>
		auto ReportException(std::string_view detail, const LogFn &log) noexcept -> int {
			std::array<char, kExceptionBufferSize> buffer{};
			const std::string_view                 message = FormatExceptionMessage(detail, buffer);
			const auto                             emit_log = [&] noexcept -> void {
				try {
					log(message);
				} catch (...) {
					// Best effort: never abort while reporting an error.
					return;
				}
			};
			if constexpr (std::same_as<std::remove_cvref_t<LogFn>,
			                           std::function<void(std::string_view)>>) {
				if (log) {
					emit_log();
				}
			} else {
				emit_log();
			}
			try {
				std::cerr << message << "\n";
			} catch (...) {
				// Best effort: stderr may fail or throw under a custom stream buffer.
				return 1;
			}
			return 1;
		}

		template <typename Operation, typename LogFn>
		auto RunGuardedImpl(const Operation &operation, const LogFn &log) noexcept -> int {
			try {
				return operation();
			} catch (const std::exception &error) {
				const char *const detail = error.what();
				return ReportException(
				    detail != nullptr ? std::string_view(detail) : std::string_view{}, log);
			} catch (...) {
				return ReportException({}, log);
			}
		}

		// An AF_UNIX SOCK_SEQPACKET socket, as PAM hands the helper on descriptor 3.
		auto ValidSeqpacketSocket(int fd) -> bool {
			struct stat stat{};
			if (fstat(fd, &stat) != 0 || !S_ISSOCK(stat.st_mode)) {
				return false;
			}
			int       type = 0;
			socklen_t size = sizeof(type);
			if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) != 0 || type != SOCK_SEQPACKET) {
				return false;
			}
			sockaddr address{};
			size = sizeof(address);
			return getsockname(fd, &address, &size) == 0 && address.sa_family == AF_UNIX;
		}

	}  // namespace

	void SyslogDiagnosticLines(std::string_view text) {
		if (text.empty()) {
			return;
		}
		openlog("howdy-auth-helper", LOG_PID, LOG_AUTHPRIV);
		for (const auto line : std::views::split(text, '\n')) {
			if (!line.empty()) {
				const auto line_len = static_cast<int>(std::min(
				    line.size(), static_cast<std::size_t>(std::numeric_limits<int>::max())));
				syslog(LOG_WARNING, "%.*s", line_len, line.data());
			}
		}
		closelog();
	}

	auto RunWithQuietDiagnostics(const std::function<bool()>                 &operation,
	                             const std::function<void(std::string_view)> &log) -> bool {
		ScopedStreamCapture capture(std::cerr, log);
		bool                succeeded = false;
		try {
			succeeded = operation();
		} catch (...) {
			std::cerr << capture.Release();
			throw;
		}
		const auto diagnostics = capture.Release();
		if (!succeeded) {
			std::cerr << diagnostics;
		}
		return succeeded;
	}

	namespace {

		auto OpenForUser(const std::string &user) -> int {
			namespace message = howdy::native::runtime_descriptor_message;
			if (geteuid() != 0) {
				return FailAndLog("howdy-auth-helper must be installed setuid root");
			}
			if (!howdy::native::IsValidModelUserName(user)) {
				return FailAndLog(howdy::native::kInvalidUserNameMessage);
			}
			const auto entry = howdy::native::LookupPasswdByUid(getuid());
			if (!entry.has_value()) {
				return FailAndLog("Failed to resolve calling user");
			}
			if (entry->name.empty() || user != entry->name) {
				return FailAndLog(
				    "howdy-auth-helper can only open runtime files for the calling user");
			}
			if (!ValidSeqpacketSocket(message::kHelperSocketFd)) {
				return FailAndLog(
				    "howdy-auth-helper requires an inherited descriptor socket on descriptor 3");
			}

			const bool sent = RunWithQuietDiagnostics([&] -> bool {
				// The setuid helper ignores HOWDY_CONFIG and HOWDY_USER_MODELS_DIR for an
				// unprivileged caller (runtime_paths.cpp), so these are the installed locations.
				const auto opened =
				    OpenRuntimeDescriptors(user, {
				                                     .config      = ResolveConfigPath(),
				                                     .user_models = howdy::native::UserModelStore({
				                                         .models_dir = ResolveUserModelsDir(),
				                                         .owner_uid  = 0,
				                                     }),
				                                     .config_owner_uid = 0,
				                                 });
				if (!opened.ok) {
					std::cerr << opened.error_message << "\n";
					return false;
				}
				if (!opened.model_message.empty()) {
					std::cerr << opened.model_message << "\n";
				}
				if (!message::SendMessage(message::kHelperSocketFd,
				                          {
				                              .model_state = opened.model_state,
				                              .config_fd   = opened.config.Get(),
				                              .model_fd    = opened.model.Get(),
				                          })) {
					std::cerr << "Failed to send the runtime descriptors\n";
					return false;
				}
				return true;
			});
			return sent ? 0 : 1;
		}

		auto RunCommand(int argc, char **argv) -> int {
			if (argc <= 0 || argv == nullptr) {
				Usage("howdy-auth-helper");
				return 1;
			}
			const auto arguments = std::span(argv, static_cast<std::size_t>(argc));
			if (std::ranges::any_of(arguments, [](const char *argument) -> bool {
				    return argument == nullptr;
			    })) {
				Usage("howdy-auth-helper");
				return 1;
			}
			const char *program = arguments.front();
			if (arguments.size() == 2) {
				const std::string_view option = arguments.subspan(1, 1).front();
				if (option == "--help" || option == "-h") {
					Usage(program);
					return 0;
				}
			}
			if (arguments.size() == 3 &&
			    std::string_view(arguments.subspan(1, 1).front()) ==
			        howdy::native::runtime_descriptor_message::kOpenSubcommand) {
				return OpenForUser(arguments.subspan(2, 1).front());
			}
			Usage(program);
			return 1;
		}

	}  // namespace

	auto RunGuarded(const std::function<int()>                  &operation,
	                const std::function<void(std::string_view)> &log) noexcept -> int {
		if (!operation) {
			return ReportException("missing auth-helper operation", log);
		}
		return RunGuardedImpl(operation, log);
	}

	auto Run(int argc, char **argv) noexcept -> int {
		return RunGuardedImpl(
		    [argc, argv] -> int {
			    return RunCommand(argc, argv);
		    },
		    SyslogDiagnosticLines);
	}

}  // namespace howdy::native::auth_helper::command
