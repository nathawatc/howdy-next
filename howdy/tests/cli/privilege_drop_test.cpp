#include "cli/privilege_drop.hpp"
#include "test_support.hpp"

#include <cerrno>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <sys/types.h>

namespace {

	using howdy::native::cli_internal::DropPrivilegesPermanently;
	using howdy::native::cli_internal::PrivilegeDropOperations;
	using howdy::test::Expect;

	constexpr uid_t kUserUid = 1000;
	constexpr gid_t kUserGid = 1001;

	// Emulates the kernel rule the ordering depends on: only an effective UID 0 may change IDs.
	struct FakeCredentials {
		uid_t                    uid  = 0;
		uid_t                    euid = 0;
		gid_t                    gid  = 0;
		gid_t                    egid = 0;
		std::vector<std::string> calls;
		std::string              initgroups_user;
		gid_t                    initgroups_gid = 0;
		std::string              fail;   // operation that fails with EIO
		std::string              stale;  // ID that keeps its old value: "uid", "euid", ...
	};

	auto Fake(void *context) -> FakeCredentials & {
		return *static_cast<FakeCredentials *>(context);
	}

	auto Change(FakeCredentials &fake, std::string_view name) -> int {
		fake.calls.emplace_back(name);
		if (fake.euid != 0) {
			errno = EPERM;
			return -1;
		}
		if (fake.fail == name) {
			errno = EIO;
			return -1;
		}
		return 0;
	}

	auto FakeInitgroups(void *context, const char *user, gid_t gid) -> int {
		auto &fake           = Fake(context);
		fake.initgroups_user = user;
		fake.initgroups_gid  = gid;
		return Change(fake, "initgroups");
	}

	auto FakeSetgid(void *context, gid_t gid) -> int {
		auto &fake = Fake(context);
		if (Change(fake, "setgid") != 0) {
			return -1;
		}
		fake.gid  = fake.stale == "gid" ? fake.gid : gid;
		fake.egid = fake.stale == "egid" ? fake.egid : gid;
		return 0;
	}

	auto FakeSetuid(void *context, uid_t uid) -> int {
		auto &fake = Fake(context);
		if (Change(fake, "setuid") != 0) {
			return -1;
		}
		fake.uid  = fake.stale == "uid" ? fake.uid : uid;
		fake.euid = fake.stale == "euid" ? fake.euid : uid;
		return 0;
	}

	auto OperationsFor(FakeCredentials &fake) -> PrivilegeDropOperations {
		return {
		    .context    = &fake,
		    .initgroups = FakeInitgroups,
		    .setgid     = FakeSetgid,
		    .setuid     = FakeSetuid,
		    .getuid     = [](void *context) -> uid_t {
			    return Fake(context).uid;
		    },
		    .geteuid = [](void *context) -> uid_t {
			    return Fake(context).euid;
		    },
		    .getgid = [](void *context) -> gid_t {
			    return Fake(context).gid;
		    },
		    .getegid = [](void *context) -> gid_t {
			    return Fake(context).egid;
		    },
		};
	}

	auto Run(const PrivilegeDropOperations &operations) -> bool {
		return DropPrivilegesPermanently({.uid = kUserUid, .gid = kUserGid, .name = "user"},
		                                 operations);
	}

	auto DropTakesOnUserIdentity() -> bool {
		FakeCredentials fake;
		bool            ok = Expect(Run(OperationsFor(fake)), "privilege drop succeeds");
		ok &= Expect(fake.calls == std::vector<std::string>{"initgroups", "setgid", "setuid"},
		             "privilege drop changes groups, then GID, then UID");
		ok &= Expect(fake.initgroups_user == "user" && fake.initgroups_gid == kUserGid,
		             "privilege drop loads the user's groups");
		ok &= Expect(fake.uid == kUserUid && fake.euid == kUserUid && fake.gid == kUserGid &&
		                 fake.egid == kUserGid,
		             "privilege drop changes every ID");
		return ok;
	}

	auto StepFailureStopsTheDrop() -> bool {
		bool                     ok = true;
		std::vector<std::string> expected_calls;
		for (const auto *step : {"initgroups", "setgid", "setuid"}) {
			expected_calls.emplace_back(step);
			FakeCredentials fake;
			fake.fail = step;
			ok &= Expect(!Run(OperationsFor(fake)), fake.fail + " failure fails the drop");
			ok &= Expect(fake.calls == expected_calls, fake.fail + " failure stops later steps");
		}
		return ok;
	}

	auto UnverifiedIdsFailTheDrop() -> bool {
		bool ok = true;
		for (const auto *id : {"uid", "euid", "gid", "egid"}) {
			FakeCredentials fake;
			fake.stale = id;
			ok &= Expect(!Run(OperationsFor(fake)), fake.stale + " left unchanged fails the drop");
		}
		return ok;
	}

	auto NonRootCannotDrop() -> bool {
		FakeCredentials fake{.uid = kUserUid, .euid = kUserUid};
		return Expect(!Run(OperationsFor(fake)) &&
		                  fake.calls == std::vector<std::string>{"initgroups"},
		              "a drop without root fails at the first step");
	}

	auto MissingOperationsFailClosed() -> bool {
		const std::vector<std::function<void(PrivilegeDropOperations &)>> removals{
		    [](auto &operations) -> void {
			    operations.initgroups = nullptr;
		    },
		    [](auto &operations) -> void {
			    operations.setgid = nullptr;
		    },
		    [](auto &operations) -> void {
			    operations.setuid = nullptr;
		    },
		    [](auto &operations) -> void {
			    operations.getuid = nullptr;
		    },
		    [](auto &operations) -> void {
			    operations.geteuid = nullptr;
		    },
		    [](auto &operations) -> void {
			    operations.getgid = nullptr;
		    },
		    [](auto &operations) -> void {
			    operations.getegid = nullptr;
		    },
		};
		bool ok = true;
		for (const auto &remove : removals) {
			FakeCredentials fake;
			auto            operations = OperationsFor(fake);
			remove(operations);
			ok &= Expect(!Run(operations) && fake.calls.empty(),
			             "a missing operation fails before any change");
		}
		return ok;
	}

	auto SystemOperationsAreComplete() -> bool {
		const auto operations = howdy::native::cli_internal::SystemPrivilegeDropOperations();
		return Expect(operations.initgroups != nullptr && operations.setgid != nullptr &&
		                  operations.setuid != nullptr && operations.getuid != nullptr &&
		                  operations.geteuid != nullptr && operations.getgid != nullptr &&
		                  operations.getegid != nullptr,
		              "system operations are complete");
	}

}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= DropTakesOnUserIdentity();
	ok &= StepFailureStopsTheDrop();
	ok &= UnverifiedIdsFailTheDrop();
	ok &= NonRootCannotDrop();
	ok &= MissingOperationsFailClosed();
	ok &= SystemOperationsAreComplete();
	return ok ? 0 : 1;
}
