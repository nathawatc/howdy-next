#include "protocol/compare_descriptors.hpp"
#include "protocol/compare_verdict.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <pwd.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/stat.h>

// Stands in for howdy-compare in the installed e2e: checks what the installed PAM module hands
// compare in descriptor mode, then reports success the way compare does.
namespace {

	namespace descriptors = howdy::native::compare_descriptors;

	auto Announced(const char *name, std::string_view value) -> bool {
		const char *const announced = getenv(name);
		return announced != nullptr && std::string_view(announced) == value;
	}

	// PAM passes compare its end of the verdict socket (protocol/compare_verdict.hpp).
	auto VerdictSocketInherited() -> bool {
		using howdy::native::compare_verdict::kFd;
		struct stat metadata{};
		int         domain = -1;
		int         type   = -1;
		socklen_t   length = sizeof(domain);
		if (!Announced(howdy::native::compare_verdict::kEnvironmentName,
		               howdy::native::compare_verdict::kEnvironmentValue) ||
		    fstat(kFd, &metadata) != 0 || !S_ISSOCK(metadata.st_mode) ||
		    getsockopt(kFd, SOL_SOCKET, SO_DOMAIN, &domain, &length) != 0 || domain != AF_UNIX) {
			return false;
		}
		length = sizeof(type);
		return getsockopt(kFd, SOL_SOCKET, SO_TYPE, &type, &length) == 0 && type == SOCK_SEQPACKET;
	}

	// A runtime file PAM passes (config or model): read-only, regular, root-owned, readable
	// through the descriptor, but not reopenable by this user through the path it came from.
	auto RuntimeDescriptorInherited(int fd, const char *name, std::string_view value) -> bool {
		struct stat metadata{};
		const int   flags = fcntl(fd, F_GETFL);
		char        byte  = 0;
		if (!Announced(name, value) || flags < 0 || (flags & O_ACCMODE) != O_RDONLY ||
		    fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_uid != 0 ||
		    pread(fd, &byte, 1, 0) != 1) {
			return false;
		}
		std::array<char, 4096> target{};
		const std::string      link = "/proc/self/fd/" + std::to_string(fd);
		const ssize_t          size = readlink(link.c_str(), target.data(), target.size() - 1);
		if (size <= 0) {
			return false;
		}
		const int reopened = open(target.data(), O_RDONLY | O_CLOEXEC);
		if (reopened >= 0) {
			close(reopened);
			return false;
		}
		return errno == EACCES;
	}

	auto Fail(const char *reason) -> int {
		std::cerr << "Installed compare probe: " << reason << '\n';
		return 1;
	}

}  // namespace

auto main(int argc, char **argv) -> int {
	const auto arguments =
	    std::span(argv, argc > 0 ? static_cast<std::size_t>(argc) : std::size_t{0});
	if (arguments.size() != 2) {
		return Fail("unexpected compare arguments");
	}
	const char *user = arguments.back();
	if (!VerdictSocketInherited()) {
		return Fail("compare did not inherit the verdict socket");
	}
	if (!RuntimeDescriptorInherited(descriptors::kConfigFd, descriptors::kConfigEnvironmentName,
	                                descriptors::kConfigEnvironmentValue)) {
		return Fail("compare did not inherit a usable config descriptor");
	}
	if (!RuntimeDescriptorInherited(descriptors::kModelFd, descriptors::kModelEnvironmentName,
	                                descriptors::kModelEnvironmentValue)) {
		return Fail("compare did not inherit a usable model descriptor");
	}
	for (int fd = descriptors::kFirstUnusedFd; fd < 128; ++fd) {
		if (fcntl(fd, F_GETFD) != -1 || errno != EBADF) {
			return Fail("inherited descriptor above the model descriptor");
		}
	}

	const passwd *account = getpwnam(user);
	if (account == nullptr || account->pw_uid == 0 || getuid() != account->pw_uid ||
	    geteuid() != account->pw_uid || getgid() != account->pw_gid ||
	    getegid() != account->pw_gid) {
		return Fail("compare did not run as the selected non-root user");
	}
	if (setuid(0) != -1 || errno != EPERM) {
		return Fail("compare can regain root");
	}

	if (environ == nullptr) {
		return Fail("compare environment was not isolated");
	}
	const std::vector<std::string> expected = {
	    "OPENBLAS_NUM_THREADS=1",
	    "OMP_NUM_THREADS=1",
	    howdy::native::compare_verdict::kEnvironmentEntry,
	    descriptors::kConfigEnvironmentEntry,
	    descriptors::kModelEnvironmentEntry,
	};
	// Walk to the terminating nullptr so a shorter or longer environment is a plain mismatch,
	// never an out-of-bounds read.
	std::vector<std::string> environment;
	for (std::size_t index = 0; environ[index] != nullptr; ++index) {
		environment.emplace_back(environ[index]);
	}
	if (environment != expected) {
		return Fail("compare environment was not isolated");
	}

	std::cout << "HOWDY_E2E_COMPARE_PROBE_OK\n" << std::flush;
	// Report success on the verdict socket as compare does, as the last thing before exiting.
	const unsigned char verdict = 0;
	if (write(howdy::native::compare_verdict::kFd, &verdict, sizeof(verdict)) != 1) {
		return Fail("compare could not report its verdict");
	}
	return EXIT_SUCCESS;
}
