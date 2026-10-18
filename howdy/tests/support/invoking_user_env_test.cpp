#include "support/invoking_user.hpp"
#include "support/invoking_user_env.hpp"
#include "test_support.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <pwd.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/types.h>

namespace {

	using howdy::test::Expect;

	struct PasswdFixture {
		uid_t       uid;
		gid_t       gid;
		std::string name;
		std::string home;
		std::string shell;
	};

	auto CurrentPasswdFixture() -> std::optional<PasswdFixture> {
		const passwd *pwd = getpwuid(getuid());
		if (pwd == nullptr || pwd->pw_name == nullptr || std::string_view(pwd->pw_name).empty()) {
			return std::nullopt;
		}
		return PasswdFixture{
		    .uid   = pwd->pw_uid,
		    .gid   = pwd->pw_gid,
		    .name  = pwd->pw_name,
		    .home  = pwd->pw_dir != nullptr ? pwd->pw_dir : "",
		    .shell = pwd->pw_shell != nullptr ? pwd->pw_shell : "",
		};
	}

	class ScopedWrapperEnvironment {
	public:
		ScopedWrapperEnvironment(ScopedWrapperEnvironment &&)                     = delete;
		auto operator=(ScopedWrapperEnvironment &&) -> ScopedWrapperEnvironment & = delete;

		ScopedWrapperEnvironment()
		    : original_{} {
			for (std::size_t index = 0; index < kNames.size(); ++index) {
				if (const char *value = std::getenv(kNames.at(index)); value != nullptr) {
					original_.at(index)     = value;
					had_original_.at(index) = true;
				}
				(void)unsetenv(kNames.at(index));
			}
		}

		ScopedWrapperEnvironment(const ScopedWrapperEnvironment &)                     = delete;
		auto operator=(const ScopedWrapperEnvironment &) -> ScopedWrapperEnvironment & = delete;

		~ScopedWrapperEnvironment() noexcept {
			for (std::size_t index = 0; index < kNames.size(); ++index) {
				if (had_original_.at(index)) {
					(void)setenv(kNames.at(index), original_.at(index).c_str(), 1);
				} else {
					(void)unsetenv(kNames.at(index));
				}
			}
		}

		static void Clear() {
			for (const char *name : kNames) {
				(void)unsetenv(name);
			}
		}

		static void Set(const char *name, const std::string &value) {
			(void)setenv(name, value.c_str(), 1);
		}

	private:
		static constexpr std::array kNames = {
		    howdy::native::kSudoUserEnvironmentVariable,
		    howdy::native::kSudoUidEnvironmentVariable,
		    howdy::native::kSudoGidEnvironmentVariable,
		    howdy::native::kDoasUserEnvironmentVariable,
		    howdy::native::kPkexecUidEnvironmentVariable,
		};
		std::array<std::string, kNames.size()> original_;
		std::array<bool, kNames.size()>        had_original_{};
	};

	auto ExpectResolved(const howdy::native::InvokingIdentityResult &result,
	                    const PasswdFixture &fixture, std::string_view description) -> bool {
		return Expect(result.status == howdy::native::InvokingIdentityStatus::kResolved &&
		                  result.user.has_value() && result.user->uid == fixture.uid &&
		                  result.user->gid == fixture.gid && result.user->name == fixture.name &&
		                  result.user->home == fixture.home && result.user->shell == fixture.shell,
		              description);
	}

	auto RunInvokingIdentityTests() -> bool {
		const auto fixture = CurrentPasswdFixture();
		if (!fixture.has_value()) {
			return Expect(false, "resolver obtains a passwd fixture");
		}

		ScopedWrapperEnvironment const environment;
		bool                           ok   = true;
		const auto                    &user = *fixture;

		ScopedWrapperEnvironment::Clear();
		const auto no_wrapper = howdy::native::ResolveInvokingIdentity();
		ok &=
		    Expect(no_wrapper.status == howdy::native::InvokingIdentityStatus::kNoWrapperIdentity &&
			           !no_wrapper.user.has_value(),
			       "no wrapper metadata is represented explicitly");

		ScopedWrapperEnvironment::Set(howdy::native::kDoasUserEnvironmentVariable, user.name);
		ok &= ExpectResolved(howdy::native::ResolveInvokingIdentity(), user,
		                     "valid doas identity resolves passwd fields");

		ScopedWrapperEnvironment::Clear();
		ScopedWrapperEnvironment::Set(howdy::native::kDoasUserEnvironmentVariable, user.name);
		ScopedWrapperEnvironment::Set(howdy::native::kSudoGidEnvironmentVariable, "0");
		ok &= Expect(howdy::native::ResolveInvokingIdentity().status ==
		                 howdy::native::InvokingIdentityStatus::kConflicting,
		             "doas and sudo GID metadata conflicts");

		ScopedWrapperEnvironment::Clear();
		ScopedWrapperEnvironment::Set(howdy::native::kDoasUserEnvironmentVariable, user.name);
		ScopedWrapperEnvironment::Set(howdy::native::kSudoUserEnvironmentVariable, "forged-user");
		ok &= Expect(howdy::native::ResolveInvokingIdentity().status ==
		                 howdy::native::InvokingIdentityStatus::kConflicting,
		             "doas and sudo username metadata conflicts");

		ScopedWrapperEnvironment::Clear();
		ScopedWrapperEnvironment::Set(howdy::native::kSudoUserEnvironmentVariable, user.name);
		ScopedWrapperEnvironment::Set(howdy::native::kSudoUidEnvironmentVariable,
		                              std::to_string(user.uid));
		ScopedWrapperEnvironment::Set(howdy::native::kSudoGidEnvironmentVariable,
		                              std::to_string(user.gid));
		ok &= ExpectResolved(howdy::native::ResolveInvokingIdentity(), user,
		                     "valid sudo tuple resolves passwd fields");

		ScopedWrapperEnvironment::Set(howdy::native::kSudoUserEnvironmentVariable, "forged-user");
		ok &= Expect(howdy::native::ResolveInvokingIdentity().status ==
		                 howdy::native::InvokingIdentityStatus::kInvalid,
		             "mismatched sudo username is rejected");

		ScopedWrapperEnvironment::Set(howdy::native::kSudoUserEnvironmentVariable, user.name);
		const auto different_uid = user.uid == 0 ? static_cast<uid_t>(1) : static_cast<uid_t>(0);
		ScopedWrapperEnvironment::Set(howdy::native::kSudoUidEnvironmentVariable,
		                              std::to_string(different_uid));
		ok &= Expect(howdy::native::ResolveInvokingIdentity().status ==
		                 howdy::native::InvokingIdentityStatus::kInvalid,
		             "mismatched sudo UID is rejected");

		ScopedWrapperEnvironment::Set(howdy::native::kSudoUidEnvironmentVariable,
		                              std::to_string(user.uid));
		const auto different_gid = user.gid == 0 ? static_cast<gid_t>(1) : static_cast<gid_t>(0);
		ScopedWrapperEnvironment::Set(howdy::native::kSudoGidEnvironmentVariable,
		                              std::to_string(different_gid));
		auto alternate_user = user;
		alternate_user.gid  = different_gid;
		ok &= ExpectResolved(howdy::native::ResolveInvokingIdentity(), alternate_user,
		                     "alternate sudo GID preserves invoking group context");

		const auto gid_overflow = std::to_string(std::numeric_limits<gid_t>::max()) + "0";
		for (const auto &gid :
		     std::array<std::string, 4>{"not-a-number", "-1", " 1", gid_overflow}) {
			ScopedWrapperEnvironment::Set(howdy::native::kSudoGidEnvironmentVariable, gid);
			ok &= Expect(howdy::native::ResolveInvokingIdentity().status ==
			                 howdy::native::InvokingIdentityStatus::kInvalid,
			             "malformed sudo GID is rejected");
		}

		for (const auto &metadata : std::array<std::pair<const char *, std::string>, 4>{
		         std::pair{howdy::native::kSudoUserEnvironmentVariable, user.name},
		         std::pair{howdy::native::kSudoUidEnvironmentVariable, std::to_string(user.uid)},
		         std::pair{howdy::native::kSudoGidEnvironmentVariable, std::to_string(user.gid)},
		         std::pair{howdy::native::kSudoUidEnvironmentVariable, "not-a-number"},
		     }) {
			ScopedWrapperEnvironment::Clear();
			ScopedWrapperEnvironment::Set(metadata.first, metadata.second);
			ok &= Expect(howdy::native::ResolveInvokingIdentity().status ==
			                 howdy::native::InvokingIdentityStatus::kInvalid,
			             "partial or malformed sudo metadata is rejected");
		}

		ScopedWrapperEnvironment::Clear();
		ScopedWrapperEnvironment::Set(howdy::native::kSudoUserEnvironmentVariable, user.name);
		ScopedWrapperEnvironment::Set(howdy::native::kSudoUidEnvironmentVariable, "not-a-number");
		ScopedWrapperEnvironment::Set(howdy::native::kDoasUserEnvironmentVariable, user.name);
		ok &= Expect(howdy::native::ResolveInvokingIdentity().status ==
		                 howdy::native::InvokingIdentityStatus::kConflicting,
		             "malformed sudo metadata never falls back to doas");

		ScopedWrapperEnvironment::Clear();
		ScopedWrapperEnvironment::Set(howdy::native::kPkexecUidEnvironmentVariable,
		                              std::to_string(user.uid));
		ok &= ExpectResolved(howdy::native::ResolveInvokingIdentity(), user,
		                     "valid pkexec UID resolves passwd fields");

		ScopedWrapperEnvironment::Set(howdy::native::kSudoUidEnvironmentVariable, "0");
		ok &= Expect(howdy::native::ResolveInvokingIdentity().status ==
		                 howdy::native::InvokingIdentityStatus::kConflicting,
		             "pkexec and sudo metadata conflicts");

		ok &= Expect(!howdy::native::ParseUidEnv("+1").has_value(),
		             "UID parser rejects signed values");
		ok &=
		    Expect(!howdy::native::ParseGidEnv(" 1").has_value(), "GID parser rejects whitespace");
		ok &= Expect(!howdy::native::ParseUidEnv("999999999999999999999999").has_value(),
		             "UID parser rejects overflow");
		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= RunInvokingIdentityTests();

	setenv("XDG_RUNTIME_DIR", "/tmp/stale-runtime", 1);
	setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/tmp/stale-bus", 1);
	setenv("WAYLAND_SOCKET", "stale-wayland-fd", 1);

	howdy::native::InvokingUser user{};
	user.uid  = static_cast<uid_t>(-1);
	user.home = "/path/that/does/not/exist";

	howdy::native::ResetInvokingUserGuiEnvironment(user);

	ok &= Expect(std::getenv("XDG_RUNTIME_DIR") == nullptr,
	             "stale XDG_RUNTIME_DIR is cleared when runtime dir is unavailable");
	ok &= Expect(std::getenv("DBUS_SESSION_BUS_ADDRESS") == nullptr,
	             "stale DBUS session bus address is cleared when bus is unavailable");
	ok &= Expect(std::getenv("WAYLAND_SOCKET") == nullptr,
	             "stale Wayland socket is cleared during GUI environment preparation");

	unsetenv("XDG_RUNTIME_DIR");
	unsetenv("DBUS_SESSION_BUS_ADDRESS");
	unsetenv("WAYLAND_SOCKET");

	return ok ? 0 : 1;
}
