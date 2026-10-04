#include "auth_helper/auth_helper_acl_fake.hpp"
#include "auth_helper/runtime/internal.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <sys/stat.h>

namespace {
	using howdy::native::auth_helper::internal::RuntimeSources;
	using howdy::native::auth_helper::internal::StagedIdentity;
	using howdy::test::Expect;
	using howdy::test::WriteFile;
	using howdy::test::auth_helper::FakeAclContext;

	struct FdopendirFailureState {
		std::array<int, 8> descriptors{};
		std::size_t        count = 0;
	};

	auto GetFdopendirFailureState() -> FdopendirFailureState & {
		static FdopendirFailureState state;
		return state;
	}
}  // namespace

extern "C" auto TestFdopendirFailure(int fd) -> DIR *__asm__("__wrap_fdopendir");

extern "C" auto TestFdopendirFailure(int fd) -> DIR * {
	auto &state = GetFdopendirFailureState();
	if (state.count < state.descriptors.size()) {
		state.descriptors.at(state.count) = fd;
	}
	++state.count;
	errno = ENOMEM;
	return nullptr;
}

auto main() -> int {
	namespace fs = std::filesystem;

	bool       ok = true;
	const auto temp_root =
	    fs::current_path() / ("howdy-auth-helper-fdopendir-test-" + std::to_string(getpid()));
	const auto      source = temp_root / "source";
	const auto      models = source / "models";
	const auto      config = source / "config.ini";
	std::error_code ec;
	fs::remove_all(temp_root, ec);
	ec.clear();
	fs::create_directories(models, ec);
	ok &= Expect(!ec, "creates fdopendir failure fixture");
	if (ok) {
		ok &= Expect(WriteFile(config, "config-v1\n") && chmod(source.c_str(), 0755) == 0 &&
		                 chmod(models.c_str(), 0755) == 0 && chmod(config.c_str(), 0644) == 0,
		             "prepares secure source fixture");
	}

	const auto previous = fs::current_path();
	fs::current_path(source, ec);
	ok &= Expect(!ec, "enters source fixture");

	FakeAclContext       fake_acl;
	const StagedIdentity identity = {
	    .target_uid = getuid(),
	    .owner_uid  = geteuid(),
	    .owner_gid  = getegid(),
	};
	const RuntimeSources sources = {
	    .runtime_root    = temp_root / "runtime",
	    .config          = "./config.ini",
	    .user_models_dir = "./models",
	};
	const auto prepared = howdy::native::auth_helper::internal::PrepareRuntimeAuthFiles(
	    "alice", identity, sources, fake_acl.Operations());
	ok &= Expect(!prepared.has_value(), "fdopendir failure rejects runtime preparation");

	fs::current_path(previous, ec);
	ok &= Expect(!ec, "restores working directory");
	const auto &failure_state = GetFdopendirFailureState();
	ok &= Expect(failure_state.count > 0 && failure_state.count <= failure_state.descriptors.size(),
	             "fdopendir failure path was exercised");
	for (std::size_t index = 0;
	     index < std::min(failure_state.count, failure_state.descriptors.size()); ++index) {
		errno = 0;
		ok &= Expect(fcntl(failure_state.descriptors.at(index), F_GETFD) == -1 && errno == EBADF,
		             "failed fdopendir retains no directory descriptor");
	}

	fs::remove_all(temp_root, ec);
	return ok ? 0 : 1;
}
