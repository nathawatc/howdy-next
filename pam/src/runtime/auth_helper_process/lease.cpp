#ifndef _GNU_SOURCE
#	define _GNU_SOURCE
#endif

#include "internal.hpp"
#include "protocol/auth_helper_protocol.hpp"
#include "storage/staged_runtime_policy.hpp"
#include "support/bit_flags.hpp"
#include "support/scoped_fd.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>

#include <fcntl.h>
#include <unistd.h>

#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>  // IWYU pragma: keep

namespace {

	using howdy::pam::auth_helper_process::internal::LeaseReceiveResult;

	void CloseControlDescriptors(msghdr *message) {
		for (cmsghdr *control = CMSG_FIRSTHDR(message); control != nullptr;
		     control          = CMSG_NXTHDR(message, control)) {
			if (control->cmsg_level != SOL_SOCKET || control->cmsg_type != SCM_RIGHTS ||
			    control->cmsg_len < CMSG_LEN(0)) {
				continue;
			}
			const std::size_t count            = (control->cmsg_len - CMSG_LEN(0)) / sizeof(int);
			const auto        descriptor_bytes = std::span(CMSG_DATA(control), count * sizeof(int));
			for (std::size_t offset = 0; offset < descriptor_bytes.size(); offset += sizeof(int)) {
				int descriptor = -1;
				std::memcpy(&descriptor, descriptor_bytes.subspan(offset, sizeof(int)).data(),
				            sizeof(int));
				if (descriptor >= 0) {
					(void)close(descriptor);
				}
			}
		}
	}

}  // namespace

namespace howdy::pam::auth_helper_process::internal {

	auto ReceiveLeaseDescriptorOnce(int socket_fd, int *lease_fd) -> LeaseReceiveResult {
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
		if (received != 1 || marker != 'L' ||
		    howdy::native::HasAnyBits(message.msg_flags, MSG_TRUNC, MSG_CTRUNC)) {
			CloseControlDescriptors(&message);
			return LeaseReceiveResult::kInvalid;
		}

		cmsghdr *control = CMSG_FIRSTHDR(&message);
		if (control == nullptr || control->cmsg_level != SOL_SOCKET ||
		    control->cmsg_type != SCM_RIGHTS || control->cmsg_len != CMSG_LEN(sizeof(int)) ||
		    CMSG_NXTHDR(&message, control) != nullptr) {
			CloseControlDescriptors(&message);
			return LeaseReceiveResult::kInvalid;
		}

		int descriptor = -1;
		std::memcpy(&descriptor, CMSG_DATA(control), sizeof(descriptor));
		howdy::native::ScopedFd received_fd(descriptor);
		// Defence in depth: MSG_CMSG_CLOEXEC should already have set FD_CLOEXEC.
		const int descriptor_flags = received_fd.Valid() ? fcntl(received_fd.Get(), F_GETFD) : -1;
		if (!received_fd.Valid() || descriptor_flags < 0 ||
		    !howdy::native::HasAnyBits(descriptor_flags, FD_CLOEXEC)) {
			return LeaseReceiveResult::kInvalid;
		}

		*lease_fd = received_fd.Release();
		return LeaseReceiveResult::kReceived;
	}

	auto LeaseSocketHasCleanEof(int socket_fd) -> bool {
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
		if (received < 0) {
			return false;
		}
		const bool clean = received == 0 && CMSG_FIRSTHDR(&message) == nullptr &&
		                   !howdy::native::HasAnyBits(message.msg_flags, MSG_TRUNC, MSG_CTRUNC);
		CloseControlDescriptors(&message);
		return clean;
	}

	auto ValidateLeaseDescriptor(int lease_fd, const std::filesystem::path &root_dir,
	                             uid_t owner_uid) -> bool {
		if (lease_fd < 0 || root_dir.empty() || root_dir.filename().empty()) {
			return false;
		}

		const int  flags = fcntl(lease_fd, F_GETFL);
		const auto policy =
		    howdy::native::GetStagedRuntimePolicy(howdy::native::StagedRuntimeRole::kLock);
		struct stat lease_stat{};
		if (flags < 0 || howdy::native::MaskedBits(flags, O_ACCMODE) != O_RDONLY ||
		    !policy.exact_link_count.has_value() || fstat(lease_fd, &lease_stat) != 0 ||
		    !S_ISREG(lease_stat.st_mode) || lease_stat.st_uid != owner_uid ||
		    lease_stat.st_nlink != *policy.exact_link_count ||
		    howdy::native::MaskedBits(lease_stat.st_mode, 07777) != policy.mode) {
			return false;
		}

		const howdy::native::ScopedFd parent_fd(
		    open(root_dir.parent_path().c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
		if (!parent_fd.Valid()) {
			return false;
		}
		const auto lock_name =
		    howdy::native::auth_helper_protocol::PreparedRuntimeGenerationLockPath(root_dir)
		        .filename()
		        .string();
		struct stat path_stat{};
		const bool  identity_ok =
		    fstatat(parent_fd.Get(), lock_name.c_str(), &path_stat, AT_SYMLINK_NOFOLLOW) == 0 &&
		    S_ISREG(path_stat.st_mode) && path_stat.st_uid == owner_uid &&
		    path_stat.st_nlink == *policy.exact_link_count &&
		    howdy::native::MaskedBits(path_stat.st_mode, 07777) == policy.mode &&
		    path_stat.st_dev == lease_stat.st_dev && path_stat.st_ino == lease_stat.st_ino;
		return identity_ok && flock(lease_fd, LOCK_SH | LOCK_NB) == 0;
	}

}  // namespace howdy::pam::auth_helper_process::internal
