#include "internal.hpp"
#include "protocol/compare_descriptors.hpp"
#include "protocol/compare_verdict.hpp"
#include "support/bit_flags.hpp"
#include "support/eintr.hpp"
#include "support/scoped_fd.hpp"

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/stat.h>

namespace howdy::native::compare_entrypoint_private {

	// The verdict socket PAM handed compare (protocol/compare_verdict.hpp), if any: the
	// environment announces it and the descriptor is an AF_UNIX SOCK_SEQPACKET socket open for
	// reading and writing. Checked before compare opens anything, so the descriptor cannot be one
	// of compare's own files.
	auto FindVerdictSocket() noexcept -> VerdictSocket {
		using howdy::native::compare_verdict::kFd;
		const char *const descriptor =
		    std::getenv(howdy::native::compare_verdict::kEnvironmentName);
		if (descriptor == nullptr ||
		    std::string_view(descriptor) != howdy::native::compare_verdict::kEnvironmentValue) {
			return {};
		}
		struct stat status{};
		const int   flags = fcntl(kFd, F_GETFL);
		if (flags < 0 ||
		    howdy::native::MaskedBits(flags, O_ACCMODE) !=
		        howdy::native::BitRepresentation(O_RDWR) ||
		    fstat(kFd, &status) != 0 || !S_ISSOCK(status.st_mode)) {
			return {};
		}
		int       domain = -1;
		int       type   = -1;
		socklen_t length = sizeof(domain);
		if (getsockopt(kFd, SOL_SOCKET, SO_DOMAIN, &domain, &length) != 0 || domain != AF_UNIX) {
			return {};
		}
		length = sizeof(type);
		if (getsockopt(kFd, SOL_SOCKET, SO_TYPE, &type, &length) != 0 || type != SOCK_SEQPACKET) {
			return {};
		}
		return {.announced = true, .device = status.st_dev, .inode = status.st_ino};
	}

	// Also reports the exit code on the verdict socket, so PAM still learns it if the application
	// reaps compare first. Best effort: a failed write leaves PAM with only the exit status, which
	// fails closed when it is gone.
	auto ReportVerdict(const VerdictSocket &socket, int exit_code) noexcept -> int {
		struct stat status{};
		if (socket.announced && fstat(howdy::native::compare_verdict::kFd, &status) == 0 &&
		    S_ISSOCK(status.st_mode) && status.st_dev == socket.device &&
		    status.st_ino == socket.inode) {
			const auto verdict = static_cast<unsigned char>(exit_code);
			(void)howdy::native::RetryOnEintr([&] -> ssize_t {
				return write(howdy::native::compare_verdict::kFd, &verdict, sizeof(verdict));
			});
		}
		return exit_code;
	}

	// Takes the descriptors the environment announces, before compare opens anything, so neither
	// number can belong to one of compare's own files. Each descriptor announced with its exact
	// value is compare's from here on and is closed on every path; type, owner, mode and link
	// checks are left to the loaders.
	auto TakeRuntimeDescriptors(bool verdict_announced) noexcept -> RuntimeDescriptors {
		namespace descriptors  = howdy::native::compare_descriptors;
		const auto env_present = [](const char *name) -> bool {
			return std::getenv(name) != nullptr;
		};
		const auto announced = [](const char *name, std::string_view expected) -> bool {
			const char *const value = std::getenv(name);
			return value != nullptr && std::string_view(value) == expected;
		};
		const auto take = [](int fd, howdy::native::ScopedFd *owner) -> bool {
			if (fcntl(fd, F_GETFD) < 0) {
				return false;
			}
			owner->Reset(fd);
			return true;
		};
		RuntimeDescriptors result;
		result.protocol_mode = verdict_announced ||
		                       env_present(howdy::native::compare_verdict::kEnvironmentName) ||
		                       env_present(descriptors::kConfigEnvironmentName) ||
		                       env_present(descriptors::kModelEnvironmentName);
		const bool config_announced =
		    announced(descriptors::kConfigEnvironmentName, descriptors::kConfigEnvironmentValue);
		const bool model_announced =
		    announced(descriptors::kModelEnvironmentName, descriptors::kModelEnvironmentValue);
		const bool config_open = config_announced && take(descriptors::kConfigFd, &result.config);
		const bool model_open  = model_announced && take(descriptors::kModelFd, &result.model);
		if (!config_announced || !model_announced) {
			result.mode = RuntimeDescriptors::Mode::kNotAnnounced;
		} else if (!config_open || !model_open) {
			result.mode = RuntimeDescriptors::Mode::kNotOpen;
		} else if (!verdict_announced) {
			result.mode = RuntimeDescriptors::Mode::kNoVerdict;
		} else {
			result.mode = RuntimeDescriptors::Mode::kReady;
		}
		return result;
	}

	// Why the descriptors cannot be used, or nullopt when compare may go on.
	auto RuntimeDescriptorsError(const RuntimeDescriptors &descriptors)
	    -> std::optional<std::string> {
		namespace protocol = howdy::native::compare_descriptors;
		using Mode         = RuntimeDescriptors::Mode;
		switch (descriptors.mode) {
			case Mode::kReady:
				return std::nullopt;
			case Mode::kNotAnnounced:
				return std::string(
				           "Compare needs its config and model descriptors, announced as ") +
				       protocol::kConfigEnvironmentEntry + " and " +
				       protocol::kModelEnvironmentEntry;
			case Mode::kNotOpen:
				return std::string("An announced compare descriptor is not open");
			case Mode::kNoVerdict:
				return std::string("Compare descriptors require the verdict socket");
		}
		return std::string("Invalid compare descriptors");
	}

}  // namespace howdy::native::compare_entrypoint_private
