#include "protocol/compare_verdict.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
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

namespace {
	auto ReadableRootOwnedFile(const std::filesystem::path &path) -> bool {
		struct stat metadata{};
		if (stat(path.c_str(), &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
		    metadata.st_uid != 0 ||
		    (static_cast<unsigned int>(metadata.st_mode) & 0777U) != 0640U) {
			return false;
		}
		const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		if (fd < 0) {
			return false;
		}
		char       byte     = 0;
		const bool readable = read(fd, &byte, 1) == 1;
		close(fd);
		return readable;
	}

	// PAM passes compare its end of the verdict socket (protocol/compare_verdict.hpp).
	auto VerdictSocketInherited() -> bool {
		using howdy::native::compare_verdict::kFd;
		const char *const announced = getenv(howdy::native::compare_verdict::kEnvironmentName);
		struct stat       metadata{};
		int               domain = -1;
		int               type   = -1;
		socklen_t         length = sizeof(domain);
		if (announced == nullptr ||
		    std::string_view(announced) != howdy::native::compare_verdict::kEnvironmentValue ||
		    fstat(kFd, &metadata) != 0 || !S_ISSOCK(metadata.st_mode) ||
		    getsockopt(kFd, SOL_SOCKET, SO_DOMAIN, &domain, &length) != 0 || domain != AF_UNIX) {
			return false;
		}
		length = sizeof(type);
		return getsockopt(kFd, SOL_SOCKET, SO_TYPE, &type, &length) == 0 && type == SOCK_SEQPACKET;
	}

	auto Fail(const char *reason) -> int {
		std::cerr << "Installed compare probe: " << reason << '\n';
		return 1;
	}
}  // namespace

auto main(int argc, char **argv) -> int {
	const auto arguments =
	    std::span(argv, argc > 0 ? static_cast<std::size_t>(argc) : std::size_t{0});
	if (arguments.size() != 4 || std::string_view(arguments.subspan(1).front()) != "--config") {
		return Fail("unexpected compare arguments");
	}
	const char *config_arg = arguments.subspan(2).front();
	const char *user       = arguments.back();
	if (!VerdictSocketInherited()) {
		return Fail("compare did not inherit the verdict socket");
	}
	for (int fd = howdy::native::compare_verdict::kFd + 1; fd < 128; ++fd) {
		if (fcntl(fd, F_GETFD) != -1 || errno != EBADF) {
			return Fail("inherited descriptor above the verdict socket");
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

	const char *models_env = getenv("HOWDY_USER_MODELS_DIR");
	if (models_env == nullptr || environ == nullptr || *environ == nullptr) {
		return Fail("compare environment was not isolated");
	}
	const std::vector<std::string> expected = {
	    std::string("HOWDY_USER_MODELS_DIR=") + models_env,
	    "OPENBLAS_NUM_THREADS=1",
	    "OMP_NUM_THREADS=1",
	    howdy::native::compare_verdict::kEnvironmentEntry,
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
	const std::filesystem::path config(config_arg);
	const auto                  generation      = config.parent_path();
	const auto                  generation_name = generation.filename().string();
	const auto                  expected_prefix = "pam-" + std::to_string(account->pw_uid) + "-gen";
	if (config.filename() != "config.ini" || generation.parent_path() != "/run/howdy" ||
	    !generation_name.starts_with(expected_prefix) ||
	    std::filesystem::path(models_env) != generation / "models") {
		return Fail("compare did not receive the staged runtime paths");
	}
	if (!ReadableRootOwnedFile(config) ||
	    !ReadableRootOwnedFile(generation / "models" / (std::string(user) + ".dat"))) {
		return Fail("compare cannot read staged config and model");
	}

	std::cout << "HOWDY_E2E_COMPARE_PROBE_OK\n" << std::flush;
	// Report success on the verdict socket as compare does, as the last thing before exiting.
	const unsigned char verdict = 0;
	if (write(howdy::native::compare_verdict::kFd, &verdict, sizeof(verdict)) != 1) {
		return Fail("compare could not report its verdict");
	}
	return 0;
}
