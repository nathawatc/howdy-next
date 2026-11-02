#include "paths.hpp"
#include "protocol/auth_helper_protocol.hpp"
#include "test_support.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include <sys/eventfd.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/syscall.h>

// Host conformance checks. These do not test Howdy logic; they verify the
// assumptions Howdy makes about the distribution it runs on, so a port to a new
// distro or container image fails here with a precise message instead of deep
// inside an unrelated suite.

namespace {

	namespace fs = std::filesystem;

	using howdy::test::Expect;

	constexpr std::size_t kMaximumPasswdBufferSize = 64UZ * 1024UZ;

	// compare drops privileges to the "nobody" account. Debian and Ubuntu pair it
	// with group "nogroup", Fedora and Arch with "nobody"; only the numeric ids
	// matter, and neither may be root.
	auto TestNobodyAccountIsUsableDropTarget() -> bool {
		passwd            entry{};
		passwd           *found = nullptr;
		std::vector<char> buffer(1024);
		int               error = ERANGE;
		while (error == ERANGE && buffer.size() <= kMaximumPasswdBufferSize) {
			found = nullptr;
			error = getpwnam_r("nobody", &entry, buffer.data(), buffer.size(), &found);
			if (error == ERANGE) {
				buffer.resize(buffer.size() * 2);
			}
		}
		bool ok = Expect(error == 0, "nobody account lookup succeeds");
		ok &= Expect(found != nullptr, "nobody account exists");
		if (found == nullptr) {
			return false;
		}
		ok &= Expect(found->pw_uid != 0, "nobody uid is not root");
		ok &= Expect(found->pw_gid != 0, "nobody gid is not root");
		return ok;
	}

	auto IsNormalizedAbsolutePath(std::string_view value) -> bool {
		if (value.empty() || value.front() != '/') {
			return false;
		}
		const fs::path path(value);
		return path == path.lexically_normal() && path.filename() != "" && !value.contains("//");
	}

	auto TestGeneratedPathsAreNormalizedAbsolutePaths() -> bool {
		bool ok = true;

		struct NamedPath {
			std::string_view name;
			std::string_view value;
		};

		for (const auto &[name, value] : std::array{
		         NamedPath{.name = "config path", .value = kConfiguredConfigPath},
		         NamedPath{.name = "models dir", .value = kConfiguredModelsDir},
		         NamedPath{.name = "user models dir", .value = kConfiguredUserModelsDir},
		         NamedPath{.name = "log path", .value = kConfiguredLogPath},
		         NamedPath{.name = "compare process path", .value = kCompareProcessPath},
		         NamedPath{.name = "auth helper path", .value = kAuthHelperPath},
		     }) {
			ok &= Expect(IsNormalizedAbsolutePath(value),
			             std::string("configured ") + std::string(name) +
			                 " is a normalized absolute path: " + std::string(value));
		}
		ok &= Expect(fs::path(kConfiguredConfigPath).filename() == "config.ini",
		             "configured config path names config.ini");
		ok &= Expect(fs::path(kCompareProcessPath).filename() == "howdy-compare",
		             "compare process path names howdy-compare");
		ok &= Expect(fs::path(kAuthHelperPath).filename() == "howdy-auth-helper",
		             "auth helper path names howdy-auth-helper");
		ok &= Expect(fs::path(kCompareProcessPath).parent_path().filename() == "howdy",
		             "compare process lives in a howdy helper directory");
		ok &= Expect(fs::path(kConfiguredUserModelsDir) != fs::path(kConfiguredModelsDir),
		             "user models and shipped models directories are distinct");
		return ok;
	}

	// The auth helper stages per-user runtime state under a root directory on
	// tmpfs; its parent must be a real directory (not a symlink) on every distro.
	auto TestRuntimeRootParentIsRealDirectory() -> bool {
		const auto parent =
		    howdy::native::auth_helper_protocol::PreparedRuntimeRoot().parent_path();
		struct stat info{};
		bool        ok = Expect(lstat(parent.c_str(), &info) == 0,
		                        "runtime root parent exists: " + parent.string());
		if (!ok) {
			return false;
		}
		ok &= Expect(S_ISDIR(info.st_mode),
		             "runtime root parent is a real directory (not a symlink)");
		return ok;
	}

	// staged.cpp resolves ACLs through /proc/self/fd/<n> on O_PATH descriptors.
	auto TestProcSelfFdResolvesPathDescriptors(const fs::path &directory) -> bool {
		const auto file = directory / "proc-fd-probe";
		bool       ok   = Expect(howdy::test::WriteFile(file, "probe"), "writes proc fd probe");
		const howdy::test::ScopedFd descriptor(open(file.c_str(), O_PATH | O_CLOEXEC));
		ok &= Expect(descriptor.Get() >= 0, "opens O_PATH descriptor");
		if (descriptor.Get() < 0) {
			return false;
		}
		const std::string proc_path = "/proc/self/fd/" + std::to_string(descriptor.Get());
		struct stat       by_proc{};
		struct stat       by_path{};
		ok &= Expect(stat(proc_path.c_str(), &by_proc) == 0, "/proc/self/fd entry is resolvable");
		ok &= Expect(stat(file.c_str(), &by_path) == 0, "probe file is resolvable");
		ok &= Expect(by_proc.st_ino == by_path.st_ino && by_proc.st_dev == by_path.st_dev,
		             "/proc/self/fd entry resolves to the opened file");
		return ok;
	}

	// pam_howdy waits on the compare process with pidfd_open and ppoll, and wakes
	// its worker with an eventfd. Configure does not probe the kernel, so a
	// container with a restrictive syscall filter shows up here.
	auto TestCompareProcessKernelPrimitives() -> bool {
		bool ok = true;

		const howdy::test::ScopedFd pidfd(static_cast<int>(syscall(SYS_pidfd_open, getpid(), 0)));
		ok &= Expect(pidfd.Get() >= 0, std::string("pidfd_open works: ") + std::strerror(errno));

		const howdy::test::ScopedFd event(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
		ok &= Expect(event.Get() >= 0, std::string("eventfd works: ") + std::strerror(errno));

		if (event.Get() >= 0) {
			pollfd         descriptor{.fd = event.Get(), .events = POLLIN, .revents = 0};
			const timespec timeout{.tv_sec = 0, .tv_nsec = 1'000'000};
			const int      ready = ppoll(&descriptor, 1, &timeout, nullptr);
			ok &= Expect(ready == 0, "ppoll times out on an idle eventfd");
		}
		return ok;
	}

	// Atomic staged commits rely on renameat2(RENAME_NOREPLACE); the model store
	// additionally uses RENAME_EXCHANGE and degrades to "unsupported" without it.
	auto TestAtomicRenameSupport(const fs::path &directory) -> bool {
		bool       ok     = true;
		const auto source = directory / "rename-source";
		const auto target = directory / "rename-target";
		ok &= Expect(howdy::test::WriteFile(source, "source"), "writes rename source");

#ifdef RENAME_NOREPLACE
		const int noreplace =
		    renameat2(AT_FDCWD, source.c_str(), AT_FDCWD, target.c_str(), RENAME_NOREPLACE);
		ok &= Expect(noreplace == 0,
		             std::string("renameat2(RENAME_NOREPLACE) works: ") + std::strerror(errno));
#else
		ok &= Expect(false, "RENAME_NOREPLACE is defined by the C library");
#endif

		ok &= Expect(howdy::test::WriteFile(source, "source-again"), "rewrites rename source");
#ifdef RENAME_EXCHANGE
		const int exchange =
		    renameat2(AT_FDCWD, source.c_str(), AT_FDCWD, target.c_str(), RENAME_EXCHANGE);
		if (exchange != 0) {
			ok &= howdy::test::SkipUnavailable(
			    "renameat2-exchange", std::string("renameat2(RENAME_EXCHANGE) unsupported here: ") +
			                              std::strerror(errno));
		}
#endif
		return ok;
	}

}  // namespace

auto main() -> int {
	const auto temporary = howdy::test::CreateTempDirectory("howdy-distro-conformance-");
	if (!temporary.has_value()) {
		std::cerr << "FAIL: cannot create temporary directory\n";
		return 1;
	}
	const howdy::test::TemporaryDirectory cleanup(*temporary);

	bool ok = true;
	ok &= TestNobodyAccountIsUsableDropTarget();
	ok &= TestGeneratedPathsAreNormalizedAbsolutePaths();
	ok &= TestRuntimeRootParentIsRealDirectory();
	ok &= TestProcSelfFdResolvesPathDescriptors(*temporary);
	ok &= TestCompareProcessKernelPrimitives();
	ok &= TestAtomicRenameSupport(*temporary);
	return ok ? 0 : 1;
}
