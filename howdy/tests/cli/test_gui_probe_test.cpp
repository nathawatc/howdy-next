#include "cli/test/gui_probe.hpp"
#include "support/invoking_user.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <sys/types.h>

namespace {

	using howdy::native::InvokingUser;
	using howdy::native::test_cli_internal::GuiProbeOperations;
	using howdy::native::test_cli_internal::RunGuiProbeAsInvokingUser;
	using howdy::test::Expect;
	using howdy::test::StreamRedirect;

	constexpr uid_t kUserUid = 1000;
	constexpr gid_t kUserGid = 1001;

	auto RootGroups() -> std::vector<gid_t> {
		return {0, 7};
	}

	// Emulates the kernel rule the ordering depends on: only an effective UID 0 may change IDs.
	struct FakeCredentials {
		uid_t              euid                 = 0;
		gid_t              egid                 = 0;
		std::vector<gid_t> groups               = RootGroups();
		int                group_count_override = -1;
		int                group_read_errno     = 0;
		std::string        fail;  // operation that fails with EIO, "name" or "name:root"
		bool               ignore_seteuid = false;
		bool               throw_in_probe = false;
		int                probe_calls    = 0;
		bool               probe_as_user  = false;
	};

	auto Fake(void *context) -> FakeCredentials & {
		return *static_cast<FakeCredentials *>(context);
	}

	auto Fails(const FakeCredentials &fake, std::string_view name, bool restoring) -> bool {
		return fake.fail == name || (restoring && fake.fail == std::string(name) + ":root");
	}

	auto IsRoot(const FakeCredentials &fake) -> bool {
		return fake.euid == 0 && fake.egid == 0 && fake.groups == RootGroups();
	}

	auto Fail(int error_number) -> int {
		errno = error_number;
		return -1;
	}

	auto FakeGetgroups(void *context, int size, gid_t *groups) -> int {
		auto &fake = Fake(context);
		if (Fails(fake, "getgroups", false)) {
			return Fail(EIO);
		}
		if (size == 0) {
			return fake.group_count_override >= 0 ? fake.group_count_override
			                                      : static_cast<int>(fake.groups.size());
		}
		if (fake.group_read_errno != 0) {
			return Fail(fake.group_read_errno);
		}
		if (static_cast<std::size_t>(size) < fake.groups.size()) {
			return Fail(EINVAL);
		}
		std::ranges::copy(fake.groups, std::span(groups, static_cast<std::size_t>(size)).begin());
		return static_cast<int>(fake.groups.size());
	}

	auto FakeSetgroups(void *context, std::size_t count, const gid_t *groups) -> int {
		auto &fake = Fake(context);
		if (fake.euid != 0) {
			return Fail(EPERM);
		}
		if (Fails(fake, "setgroups", true)) {
			return Fail(EIO);
		}
		const std::span values(groups, count);
		fake.groups.assign(values.begin(), values.end());
		return 0;
	}

	auto FakeInitgroups(void *context, const char * /*user*/, gid_t gid) -> int {
		auto &fake = Fake(context);
		if (fake.euid != 0) {
			return Fail(EPERM);
		}
		if (Fails(fake, "initgroups", false)) {
			return Fail(EIO);
		}
		fake.groups = {gid};
		return 0;
	}

	auto FakeSetegid(void *context, gid_t gid) -> int {
		auto &fake = Fake(context);
		if (fake.euid != 0) {
			return Fail(EPERM);
		}
		if (Fails(fake, "setegid", gid == 0)) {
			return Fail(EIO);
		}
		fake.egid = gid;
		return 0;
	}

	auto FakeSeteuid(void *context, uid_t uid) -> int {
		auto &fake = Fake(context);
		if (Fails(fake, "seteuid", uid == 0)) {
			return Fail(EIO);
		}
		if (!fake.ignore_seteuid) {
			fake.euid = uid;
		}
		return 0;
	}

	void FakeProbe(void *context, const InvokingUser &user) {
		auto &fake = Fake(context);
		++fake.probe_calls;
		fake.probe_as_user = fake.euid == user.uid && fake.egid == user.gid &&
		                     fake.groups == std::vector<gid_t>{user.gid};
		if (fake.throw_in_probe) {
			throw std::runtime_error("probe failed");
		}
	}

	auto OperationsFor(FakeCredentials &fake) -> GuiProbeOperations {
		return {
		    .context    = &fake,
		    .getgroups  = FakeGetgroups,
		    .setgroups  = FakeSetgroups,
		    .initgroups = FakeInitgroups,
		    .getegid    = [](void *context) -> gid_t {
			    return Fake(context).egid;
		    },
		    .geteuid = [](void *context) -> uid_t {
			    return Fake(context).euid;
		    },
		    .setegid = FakeSetegid,
		    .seteuid = FakeSeteuid,
		    .probe   = FakeProbe,
		};
	}

	auto Run(FakeCredentials &fake) -> std::optional<std::string> {
		return RunGuiProbeAsInvokingUser({.uid = kUserUid, .gid = kUserGid, .name = "user"},
		                                 OperationsFor(fake));
	}

	auto ProbeRunsAsUserAndRestoresRoot() -> bool {
		bool ok = true;
		for (const bool no_groups : {false, true}) {
			FakeCredentials fake;
			if (no_groups) {
				fake.groups.clear();
			}
			const auto groups = fake.groups;
			const auto error  = Run(fake);
			ok &= Expect(!error.has_value(), "GUI probe succeeds");
			ok &= Expect(fake.probe_calls == 1 && fake.probe_as_user,
			             "GUI probe runs under the invoking user's credentials");
			ok &= Expect(fake.euid == 0 && fake.egid == 0 && fake.groups == groups,
			             "GUI probe restores root credentials");
		}
		return ok;
	}

	auto SwitchFailuresNeverProbe() -> bool {
		bool ok = true;
		for (const auto *step : {"getgroups", "initgroups", "setegid", "seteuid"}) {
			FakeCredentials fake;
			fake.fail = step;
			if (fake.fail == "setegid" || fake.fail == "seteuid") {
				// Restoring uses the same call, so both the step and the restore fail.
				const auto error = Run(fake);
				ok &= Expect(error.has_value() && error->contains("Switching to invoking user") &&
				                 error->contains("; Restoring root"),
				             fake.fail + " failure reports the step and the failed restore");
			} else {
				const auto error = Run(fake);
				ok &= Expect(error.has_value() && error->contains("(errno 5)"),
				             fake.fail + " failure reports its errno");
				ok &= Expect(IsRoot(fake), fake.fail + " failure leaves root credentials");
			}
			ok &= Expect(fake.probe_calls == 0, fake.fail + " failure never probes");
		}

		FakeCredentials unverified;
		unverified.ignore_seteuid = true;
		const auto error          = Run(unverified);
		ok &= Expect(error == "Invoking user credentials did not verify for GUI probe",
		             "unverified switch fails closed");
		ok &= Expect(unverified.probe_calls == 0 && IsRoot(unverified),
		             "unverified switch restores root without probing");
		return ok;
	}

	auto GroupListChangesFailClosed() -> bool {
		bool ok = true;
		for (const int reported : {1, 3}) {  // grew (EINVAL) and shrank since the size query
			FakeCredentials fake;
			fake.group_count_override = reported;
			const auto error          = Run(fake);
			ok &= Expect(error.has_value() && error->contains("groups changed"),
			             "changed group list has its own message");
			ok &= Expect(fake.probe_calls == 0 && IsRoot(fake),
			             "changed group list fails before switching");
		}
		FakeCredentials fake;
		fake.group_read_errno = EPERM;
		const auto error      = Run(fake);
		ok &= Expect(error.has_value() &&
		                 error->starts_with("Reading root supplementary groups failed: ") &&
		                 error->contains("(errno 1)"),
		             "group read failure reports its errno");
		return ok;
	}

	auto RestoreFailuresAreReported() -> bool {
		bool ok = true;
		for (const auto *step : {"seteuid", "setegid", "setgroups"}) {
			FakeCredentials fake;
			fake.fail        = std::string(step) + ":root";
			const auto error = Run(fake);
			ok &= Expect(error.has_value() && error->starts_with("Restoring root "),
			             fake.fail + " restore failure is reported");
			ok &= Expect(fake.probe_calls == 1, fake.fail + " restore failure follows the probe");
		}
		return ok;
	}

	auto ProbeExceptionRestoresRoot() -> bool {
		bool ok = true;
		for (const bool restore_fails : {false, true}) {
			FakeCredentials fake;
			fake.throw_in_probe = true;
			fake.fail           = restore_fails ? "seteuid:root" : "";
			std::ostringstream const error;
			bool                     threw = false;
			{
				StreamRedirect const redirect(std::cerr, error.rdbuf());
				try {
					(void)Run(fake);
				} catch (const std::runtime_error &) {
					threw = true;
				}
			}
			ok &= Expect(threw, "probe exception propagates");
			ok &= Expect(IsRoot(fake) != restore_fails, "probe exception restores root");
			ok &= Expect(error.str().contains("Restoring root effective UID") == restore_fails,
			             "failed restore after a probe exception is printed");
		}
		return ok;
	}

	auto MissingOperationsFailClosed() -> bool {
		FakeCredentials fake;
		auto            operations = OperationsFor(fake);
		operations.setgroups       = nullptr;
		const auto error           = RunGuiProbeAsInvokingUser({.uid = kUserUid}, operations);
		return Expect(error == "Internal error: missing GUI probe dependency" &&
		                  fake.probe_calls == 0,
		              "missing GUI probe operation fails closed");
	}

}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= ProbeRunsAsUserAndRestoresRoot();
	ok &= SwitchFailuresNeverProbe();
	ok &= GroupListChangesFailClosed();
	ok &= RestoreFailuresAreReported();
	ok &= ProbeExceptionRestoresRoot();
	ok &= MissingOperationsFailClosed();
	return ok ? 0 : 1;
}
