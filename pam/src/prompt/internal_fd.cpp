#include "prompt/internal_fd.hpp"

#include <array>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

namespace {
	auto ProductionDuplicate(void * /*context*/, int fd, int minimum_fd) -> int {
		return fcntl(fd, F_DUPFD_CLOEXEC, minimum_fd);
	}

	auto ProductionCreatePipe(void * /*context*/, int *pipe_fds, int flags) -> int {
		return pipe2(pipe_fds, flags);
	}

	const howdy::pam::detail::InternalFdOperations kProductionOperations{
	    .duplicate   = ProductionDuplicate,
	    .create_pipe = ProductionCreatePipe,
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
	    : fd_(other.Release()) {}

	auto ScopedFd::operator=(ScopedFd &&other) noexcept -> ScopedFd & {
		if (this != &other) {
			ScopedFd old(fd_);
			fd_ = other.Release();
		}
		return *this;
	}

	auto ScopedFd::Get() const noexcept -> int {
		return fd_;
	}

	auto ScopedFd::Valid() const noexcept -> bool {
		return fd_ >= 0;
	}

	auto ScopedFd::Release() noexcept -> int {
		const int fd = fd_;
		fd_          = -1;
		return fd;
	}

	auto InternalPipe::Valid() const noexcept -> bool {
		return read.Get() > STDERR_FILENO && write.Get() > STDERR_FILENO &&
		       read.Get() != write.Get();
	}

	auto NormalizeInternalFd(ScopedFd fd, const InternalFdOperations *operations) noexcept
	    -> ScopedFd {
		if (!fd.Valid() || fd.Get() > STDERR_FILENO) {
			return fd;
		}

		const auto &active = operations == nullptr ? kProductionOperations : *operations;
		if (active.duplicate == nullptr) {
			return {};
		}
		ScopedFd normalized(active.duplicate(active.context, fd.Get(), STDERR_FILENO + 1));
		if (normalized.Get() <= STDERR_FILENO) {
			return {};
		}
		return normalized;
	}

	auto CreateInternalPipe(int flags, const InternalFdOperations *operations) noexcept
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
		pipe.read = NormalizeInternalFd(std::move(pipe.read), operations);
		if (!pipe.read.Valid()) {
			return {};
		}
		pipe.write = NormalizeInternalFd(std::move(pipe.write), operations);
		if (!pipe.Valid()) {
			return {};
		}
		return InternalPipe{.read = std::move(pipe.read), .write = std::move(pipe.write)};
	}

}  // namespace howdy::pam::detail
