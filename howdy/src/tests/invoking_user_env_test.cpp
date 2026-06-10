#include "common/invoking_user_env.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

	auto expect(bool condition, const std::string &message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << "\n";
			return false;
		}
		return true;
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	setenv("XDG_RUNTIME_DIR", "/tmp/stale-runtime", 1);
	setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/tmp/stale-bus", 1);

	howdy::native::InvokingUser user{};
	user.uid  = static_cast<uid_t>(-1);
	user.home = "/path/that/does/not/exist";

	howdy::native::reset_invoking_user_gui_environment(user);

	ok &= expect(std::getenv("XDG_RUNTIME_DIR") == nullptr,
	             "stale XDG_RUNTIME_DIR is cleared when runtime dir is unavailable");
	ok &= expect(std::getenv("DBUS_SESSION_BUS_ADDRESS") == nullptr,
	             "stale DBUS session bus address is cleared when bus is unavailable");

	unsetenv("XDG_RUNTIME_DIR");
	unsetenv("DBUS_SESSION_BUS_ADDRESS");

	return ok ? 0 : 1;
}
