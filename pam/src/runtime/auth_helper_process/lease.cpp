#ifndef _GNU_SOURCE
#	define _GNU_SOURCE
#endif

#include "internal.hpp"
#include "storage/staged_runtime_policy.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <string>
#include <unistd.h>

#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>

namespace {

	using howdy::pam::auth_helper_process::internal::HelperDeadline;
	using howdy::pam::auth_helper_process::internal::LeaseReceiveResult;

	void close_control_descriptors(msghdr *message) {
		for (cmsghdr *control = CMSG_FIRSTHDR(message); control != nullptr;
		     control          = CMSG_NXTHDR(message, control)) {
			if (control->cmsg_level != SOL_SOCKET || control->cmsg_type != SCM_RIGHTS ||
			    control->cmsg_len < CMSG_LEN(0)) {
				continue;
			}
			const std::size_t count = (control->cmsg_len - CMSG_LEN(0)) / sizeof(int);
			for (std::size_t index = 0; index < count; ++index) {
				int descriptor = -1;
				std::memcpy(&descriptor, CMSG_DATA(control) + (index * sizeof(int)), sizeof(int));
				if (descriptor >= 0) {
					(void)close(descriptor);
				}
			}
		}
	}

}  // namespace

namespace howdy::pam::auth_helper_process::internal {

	auto receive_lease_descriptor_once(int socket_fd, int *lease_fd) -> LeaseReceiveResult {
		char  marker = 0;
		iovec data{.iov_base = &marker, .iov_len = sizeof(marker)};
		alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int) * 4) + CMSG_SPACE(sizeof(ucred))>
		       control_buffer{};
		msghdr message{};
		message.msg_iov        = &data;
		message.msg_iovlen     = 1;
		message.msg_control    = control_buffer.data();
		message.msg_controllen = control_buffer.size();

		const ssize_t received = recvmsg(socket_fd, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
		if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
			return LeaseReceiveResult::kRetry;
		}
		if (received != 1 || marker != 'L' || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) {
			close_control_descriptors(&message);
			return LeaseReceiveResult::kInvalid;
		}

		cmsghdr *control = CMSG_FIRSTHDR(&message);
		if (control == nullptr || control->cmsg_level != SOL_SOCKET ||
		    control->cmsg_type != SCM_RIGHTS || control->cmsg_len != CMSG_LEN(sizeof(int)) ||
		    CMSG_NXTHDR(&message, control) != nullptr) {
			close_control_descriptors(&message);
			return LeaseReceiveResult::kInvalid;
		}

		int descriptor = -1;
		std::memcpy(&descriptor, CMSG_DATA(control), sizeof(descriptor));
		const int descriptor_flags = descriptor >= 0 ? fcntl(descriptor, F_GETFD) : -1;
		if (descriptor < 0 || descriptor_flags < 0 || (descriptor_flags & FD_CLOEXEC) == 0) {
			if (descriptor >= 0) {
				(void)close(descriptor);
			}
			return LeaseReceiveResult::kInvalid;
		}

		*lease_fd = descriptor;
		return LeaseReceiveResult::kReceived;
	}

	auto receive_lease_descriptor_until(int socket_fd, int *lease_fd, HelperDeadline deadline)
	    -> bool {
		if (socket_fd < 0 || lease_fd == nullptr) {
			return false;
		}
		*lease_fd = -1;
		while (true) {
			pollfd    descriptor{.fd = socket_fd, .events = POLLIN, .revents = 0};
			const int result = poll(&descriptor, 1, deadline_poll_timeout(deadline));
			if (result < 0 && errno == EINTR) {
				continue;
			}
			if (result <= 0 || (descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
				return false;
			}
			const auto receive_result = receive_lease_descriptor_once(socket_fd, lease_fd);
			if (receive_result == LeaseReceiveResult::kRetry) {
				continue;
			}
			return receive_result == LeaseReceiveResult::kReceived;
		}
	}

	auto lease_socket_has_clean_eof(int socket_fd) -> bool {
		char  byte = 0;
		iovec data{.iov_base = &byte, .iov_len = sizeof(byte)};
		alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int) * 4) + CMSG_SPACE(sizeof(ucred))>
		       control_buffer{};
		msghdr message{};
		message.msg_iov        = &data;
		message.msg_iovlen     = 1;
		message.msg_control    = control_buffer.data();
		message.msg_controllen = control_buffer.size();
		const ssize_t received = recvmsg(socket_fd, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
		if (received != 0) {
			if (received > 0) {
				close_control_descriptors(&message);
			}
			return false;
		}
		return CMSG_FIRSTHDR(&message) == nullptr;
	}

	auto validate_lease_descriptor(int lease_fd, const std::filesystem::path &root_dir,
	                               uid_t owner_uid) -> bool {
		if (lease_fd < 0 || root_dir.empty() || root_dir.filename().empty()) {
			return false;
		}

		const int  flags = fcntl(lease_fd, F_GETFL);
		const auto policy =
		    howdy::native::staged_runtime_policy(howdy::native::StagedRuntimeRole::kLock);
		struct stat lease_stat{};
		if (flags < 0 || (flags & O_ACCMODE) != O_RDONLY || !policy.exact_link_count.has_value() ||
		    fstat(lease_fd, &lease_stat) != 0 || !S_ISREG(lease_stat.st_mode) ||
		    lease_stat.st_uid != owner_uid || lease_stat.st_nlink != *policy.exact_link_count ||
		    (lease_stat.st_mode & 07777) != policy.mode) {
			return false;
		}

		const int parent_fd =
		    open(root_dir.parent_path().c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
		if (parent_fd < 0) {
			return false;
		}
		const std::string lock_name = root_dir.filename().string() + ".lock";
		struct stat       path_stat{};
		const bool        identity_ok =
		    fstatat(parent_fd, lock_name.c_str(), &path_stat, AT_SYMLINK_NOFOLLOW) == 0 &&
		    S_ISREG(path_stat.st_mode) && path_stat.st_uid == owner_uid &&
		    path_stat.st_nlink == *policy.exact_link_count &&
		    (path_stat.st_mode & 07777) == policy.mode && path_stat.st_dev == lease_stat.st_dev &&
		    path_stat.st_ino == lease_stat.st_ino;
		(void)close(parent_fd);
		return identity_ok && flock(lease_fd, LOCK_SH | LOCK_NB) == 0;
	}

}  // namespace howdy::pam::auth_helper_process::internal
