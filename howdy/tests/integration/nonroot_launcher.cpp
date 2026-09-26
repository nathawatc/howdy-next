#include <cstdio>
#include <span>

#include <fcntl.h>
#include <grp.h>
#include <unistd.h>

auto main(int argc, char **argv) -> int {
	if (argc < 2) {
		(void)std::fputs("Usage: howdy_nonroot_launcher <executable> [arguments...]\n", stderr);
		return 1;
	}

	const auto arguments = std::span(argv, static_cast<std::size_t>(argc) + 1).subspan(1);
	const int  fd        = open(arguments.front(), O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		std::perror("howdy_nonroot_launcher: open");
		return 1;
	}

	if (geteuid() == 0) {
		if (setgroups(0, nullptr) != 0) {
			std::perror("howdy_nonroot_launcher: setgroups");
			return 1;
		}
		if (setgid(65534) != 0) {
			std::perror("howdy_nonroot_launcher: setgid");
			return 1;
		}
		if (setuid(65534) != 0) {
			std::perror("howdy_nonroot_launcher: setuid");
			return 1;
		}
	}

	fexecve(fd, arguments.data(), environ);
	std::perror("howdy_nonroot_launcher: fexecve");
	return 1;
}
