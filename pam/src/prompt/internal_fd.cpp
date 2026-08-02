#include "internal_fd.hpp"

#include <array>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

namespace {
	auto production_duplicate(void * /*context*/, int fd, int minimum_fd) -> int {
		return fcntl(fd, F_DUPFD_CLOEXEC, minimum_fd);
	}

	auto production_create_pipe(void * /*context*/, int *pipe_fds, int flags) -> int {
		return pipe2(pipe_fds, flags);
	}

	const howdy::pam::detail::InternalFdOperations kProductionOperations{
	    .duplicate   = production_duplicate,
	    .create_pipe = production_create_pipe,
	};
}  // namespace

namespace howdy::pam::detail {

	ScopedFd::ScopedFd(int fd) noexcept
	    : fd_(fd) {}

	ScopedFd::~ScopedFd() {
		if (fd_ >= 0) {
			(void)close(fd_);
		}
	}

	ScopedFd::ScopedFd(ScopedFd &&other) noexcept
	    : fd_(other.release()) {}

	auto ScopedFd::operator=(ScopedFd &&other) noexcept -> ScopedFd & {
		if (this != &other) {
			ScopedFd old(fd_);
			fd_ = other.release();
		}
		return *this;
	}

	auto ScopedFd::get() const noexcept -> int {
		return fd_;
	}

	auto ScopedFd::valid() const noexcept -> bool {
		return fd_ >= 0;
	}

	auto ScopedFd::release() noexcept -> int {
		const int fd = fd_;
		fd_          = -1;
		return fd;
	}

	auto InternalPipe::valid() const noexcept -> bool {
		return read.get() > STDERR_FILENO && write.get() > STDERR_FILENO &&
		       read.get() != write.get();
	}

	auto normalize_internal_fd(ScopedFd fd, const InternalFdOperations *operations) noexcept
	    -> ScopedFd {
		if (!fd.valid() || fd.get() > STDERR_FILENO) {
			return fd;
		}

		const auto &active = operations == nullptr ? kProductionOperations : *operations;
		if (active.duplicate == nullptr) {
			return {};
		}
		ScopedFd normalized(active.duplicate(active.context, fd.get(), STDERR_FILENO + 1));
		if (normalized.get() <= STDERR_FILENO) {
			return {};
		}
		return normalized;
	}

	auto create_internal_pipe(int flags, const InternalFdOperations *operations) noexcept
	    -> InternalPipe {
		const auto &active = operations == nullptr ? kProductionOperations : *operations;
		if (active.create_pipe == nullptr) {
			return {};
		}

		std::array<int, 2> raw_fds{{-1, -1}};
		if (active.create_pipe(active.context, raw_fds.data(), flags) != 0) {
			return {};
		}

		InternalPipe pipe{.read = ScopedFd(raw_fds[0]), .write = ScopedFd(raw_fds[1])};
		pipe.read = normalize_internal_fd(std::move(pipe.read), operations);
		if (!pipe.read.valid()) {
			return {};
		}
		pipe.write = normalize_internal_fd(std::move(pipe.write), operations);
		if (!pipe.valid()) {
			return {};
		}
		return InternalPipe{.read = std::move(pipe.read), .write = std::move(pipe.write)};
	}

}  // namespace howdy::pam::detail
