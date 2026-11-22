#include "auth_helper/command.hpp"

#include "auth_helper/runtime_descriptors.hpp"
#include "config/runtime_paths.hpp"
#include "protocol/runtime_descriptor_message.hpp"
#include "storage/user_model_store.hpp"
#include "support/passwd_lookup.hpp"
#include "support/scoped_stream_capture.hpp"
#include "support/user_names.hpp"

#include <cstddef>
#include <functional>
#include <iostream>
#include <ranges>
#include <span>
#include <string>
#include <string_view>

#include <unistd.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syslog.h>

namespace howdy::native::auth_helper::command {
	namespace {
		auto Usage(const char *argv0) -> void {
			std::cout << "Usage: " << argv0 << " open <user>\n";
		}

		auto Fail(const std::string &message) -> int {
			std::cerr << message << "\n";
			return 1;
		}

		// PAM points `open`'s stdio at /dev/null, so its failures also go to syslog.
		auto FailAndLog(const std::string &message) -> int {
			SyslogDiagnosticLines(message);
			return Fail(message);
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
				syslog(LOG_WARNING, "%.*s", static_cast<int>(line.size()), line.data());
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
				return FailAndLog(std::string(howdy::native::kInvalidUserNameMessage));
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

	}  // namespace

	auto Run(int argc, char **argv) -> int {
		const auto arguments =
		    std::span(argv, argc > 0 ? static_cast<std::size_t>(argc) : std::size_t{0});
		const char *program = arguments.empty() ? "howdy-auth-helper" : arguments.front();
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

}  // namespace howdy::native::auth_helper::command
