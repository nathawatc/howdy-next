#pragma once

#include "protocol/compare_descriptors.hpp"
#include "support/bit_flags.hpp"
#include "support/scoped_fd.hpp"

#include <array>
#include <filesystem>

#include <fcntl.h>
#include <unistd.h>

#include <sys/socket.h>

namespace howdy::test::compare {

	// Test-owned descriptors live at or above this number, so placing one at a protocol number
	// (the verdict socket, config or model) never closes another.
	inline constexpr int kAboveProtocolFds = howdy::native::compare_descriptors::kFirstUnusedFd;

	// Moves `fd` above the protocol numbers and closes the original.
	inline auto RaiseAboveProtocol(int fd) -> howdy::native::ScopedFd {
		const howdy::native::ScopedFd original(fd);
		return howdy::native::ScopedFd(fd < 0 ? -1 : fcntl(fd, F_DUPFD_CLOEXEC, kAboveProtocolFds));
	}

	inline auto OpenAboveProtocol(const std::filesystem::path &path, int flags)
	    -> howdy::native::ScopedFd {
		return RaiseAboveProtocol(
		    open(path.c_str(), howdy::native::CombineBitsAs<int>(flags, O_CLOEXEC)));
	}

	struct VerdictPair {
		howdy::native::ScopedFd pam_end;
		howdy::native::ScopedFd compare_end;
	};

	// A verdict socket pair as PAM makes it, both ends above the protocol numbers.
	inline auto MakeVerdictPair() -> VerdictPair {
		std::array<int, 2> sockets{-1, -1};
		if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, sockets.data()) !=
		    0) {
			return {};
		}
		return {
		    .pam_end     = RaiseAboveProtocol(sockets.at(0)),
		    .compare_end = RaiseAboveProtocol(sockets.at(1)),
		};
	}

	// Puts `source` at `number` for one compare run, as PAM hands it over, or closes `number` when
	// `source` is negative. Destruction puts back whatever the test process had there; compare may
	// already have closed `number` itself.
	class ScopedDescriptorAt {
	public:
		// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
		ScopedDescriptorAt(int number, int source)
		    : number_(number)
		    , saved_(fcntl(number, F_DUPFD_CLOEXEC, kAboveProtocolFds)) {
			if (source < 0) {
				(void)close(number_);
				placed_ = true;
			} else {
				placed_ = dup2(source, number_) == number_;
			}
		}

		ScopedDescriptorAt(const ScopedDescriptorAt &)                     = delete;
		auto operator=(const ScopedDescriptorAt &) -> ScopedDescriptorAt & = delete;
		ScopedDescriptorAt(ScopedDescriptorAt &&)                          = delete;
		auto operator=(ScopedDescriptorAt &&) -> ScopedDescriptorAt &      = delete;

		~ScopedDescriptorAt() {
			if (saved_.Valid()) {
				(void)dup2(saved_.Get(), number_);
			} else {
				(void)close(number_);
			}
		}

		[[nodiscard]] auto Placed() const -> bool {
			return placed_;
		}

	private:
		int                     number_;
		howdy::native::ScopedFd saved_;
		bool                    placed_ = false;
	};

	// Whether `fd` is currently an open descriptor in this process.
	inline auto DescriptorOpen(int fd) -> bool {
		return fcntl(fd, F_GETFD) >= 0;
	}

}  // namespace howdy::test::compare
