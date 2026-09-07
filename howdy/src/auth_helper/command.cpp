#include "auth_helper/command.hpp"

#include "auth_helper/runtime.hpp"
#include "protocol/auth_helper_protocol.hpp"
#include "support/user_names.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <pwd.h>
#include <string>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>

namespace howdy::native::auth_helper::command {
	namespace {
		auto usage(const char *argv0) -> void {
			std::cout << "Usage: " << argv0 << " prepare <user>\n";
		}

		auto fail(const std::string &message) -> int {
			std::cerr << message << "\n";
			return 1;
		}

		auto valid_lease_socket(int fd) -> bool {
			struct stat stat_{};
			if (fstat(fd, &stat_) != 0 || !S_ISSOCK(stat_.st_mode)) {
				return false;
			}
			int       type = 0;
			socklen_t size = sizeof(type);
			if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) != 0 || type != SOCK_SEQPACKET) {
				return false;
			}
			sockaddr_storage address{};
			size = sizeof(address);
			return getsockname(fd, reinterpret_cast<sockaddr *>(&address), &size) == 0 &&
			       address.ss_family == AF_UNIX;
		}

		struct LeaseTransfer {
			int socket_fd;
			int lease_fd;
		};

		auto send_lease(LeaseTransfer transfer) -> bool {
			char  marker = 'L';
			iovec payload{.iov_base = &marker, .iov_len = 1};
			alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int))> control{};
			msghdr                                                     message{};
			message.msg_iov        = &payload;
			message.msg_iovlen     = 1;
			message.msg_control    = control.data();
			message.msg_controllen = control.size();
			cmsghdr *header        = CMSG_FIRSTHDR(&message);
			header->cmsg_level     = SOL_SOCKET;
			header->cmsg_type      = SCM_RIGHTS;
			header->cmsg_len       = CMSG_LEN(sizeof(int));
			std::memcpy(CMSG_DATA(header), &transfer.lease_fd, sizeof(transfer.lease_fd));
			while (true) {
				const ssize_t sent = sendmsg(transfer.socket_fd, &message, MSG_NOSIGNAL);
				if (sent == 1) {
					return true;
				}
				if (sent < 0 && errno == EINTR) {
					continue;
				}
				return false;
			}
		}
	}  // namespace

	auto print_prepared_paths(const std::filesystem::path &config_path,
	                          const std::filesystem::path &user_models_dir) -> void {
		using namespace howdy::native::auth_helper_protocol;
		std::cout << kConfigPathKey << "=" << config_path.string() << "\n";
		std::cout << kUserModelsDirKey << "=" << user_models_dir.string() << "\n";
	}

	auto prepare_for_user(const std::string &user) -> int {
		if (geteuid() != 0) {
			return fail("howdy-auth-helper must be installed setuid root");
		}
		if (!howdy::native::is_valid_model_user_name(user)) {
			return fail(howdy::native::kInvalidUserNameMessage);
		}
		const uid_t   uid   = getuid();
		const passwd *entry = getpwuid(uid);
		if (entry == nullptr) {
			return fail("Failed to resolve calling user");
		}
		if (entry->pw_name == nullptr || user != entry->pw_name) {
			return fail("howdy-auth-helper can only prepare auth files for the calling user");
		}
		if (!valid_lease_socket(auth_helper_protocol::kLeaseSocketFd)) {
			return fail("howdy-auth-helper requires an inherited lease socket on descriptor 3");
		}

		auto prepared = howdy::native::auth_helper::prepare_runtime_auth_files(
		    user, {.uid = uid, .gid = entry->pw_gid});
		if (!prepared.has_value() || prepared->lease_fd < 0) {
			return 1;
		}
		if (!send_lease({.socket_fd = auth_helper_protocol::kLeaseSocketFd,
		                 .lease_fd  = prepared->lease_fd})) {
			(void)close(prepared->lease_fd);
			return fail("Failed to transfer runtime lease");
		}
		(void)close(prepared->lease_fd);
		prepared->lease_fd = -1;
		print_prepared_paths(prepared->config_path, prepared->user_models_dir);
		if (!std::cout.good()) {
			return fail("Failed to write prepared runtime paths");
		}
		return 0;
	}

	auto run(int argc, char **argv) -> int {
		if (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
			usage(argv[0]);
			return 0;
		}
		if (argc == 3 && std::string(argv[1]) == "prepare") {
			return prepare_for_user(argv[2]);
		}
		usage(argv[0]);
		return 1;
	}

}  // namespace howdy::native::auth_helper::command
