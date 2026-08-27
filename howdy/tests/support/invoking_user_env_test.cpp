#include "support/invoking_user_env.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/un.h>

namespace {

	using howdy::test::expect;

	auto create_unix_socket(const std::filesystem::path &path) -> bool {
		const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd < 0) {
			return false;
		}

		sockaddr_un address{};
		address.sun_family     = AF_UNIX;
		const auto socket_path = path.string();
		if (socket_path.size() >= sizeof(address.sun_path)) {
			close(fd);
			return false;
		}
		std::ranges::copy(socket_path, address.sun_path);
		address.sun_path[socket_path.size()] = '\0';

		const bool created =
		    bind(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0;
		close(fd);
		return created;
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	setenv("XDG_RUNTIME_DIR", "/tmp/stale-runtime", 1);
	setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/tmp/stale-bus", 1);
	setenv("WAYLAND_SOCKET", "stale-wayland-fd", 1);

	howdy::native::InvokingUser user{};
	user.uid  = static_cast<uid_t>(-1);
	user.home = "/path/that/does/not/exist";

	howdy::native::reset_invoking_user_gui_environment(user);

	ok &= expect(std::getenv("XDG_RUNTIME_DIR") == nullptr,
	             "stale XDG_RUNTIME_DIR is cleared when runtime dir is unavailable");
	ok &= expect(std::getenv("DBUS_SESSION_BUS_ADDRESS") == nullptr,
	             "stale DBUS session bus address is cleared when bus is unavailable");
	ok &= expect(std::getenv("WAYLAND_SOCKET") == nullptr,
	             "stale Wayland socket is cleared during GUI environment preparation");

	auto  runtime_template = std::to_array("/tmp/howdy-wayland-runtime-XXXXXX");
	char *runtime_path     = mkdtemp(runtime_template.data());
	ok &= expect(runtime_path != nullptr, "Wayland discovery test creates runtime directory");
	if (runtime_path != nullptr) {
		const std::filesystem::path runtime_dir(runtime_path);
		ok &= expect(!howdy::native::find_wayland_display(runtime_dir).has_value(),
		             "runtime directory without Wayland socket has no display");

		std::ofstream(runtime_dir / "wayland-0.lock") << "lock";
		ok &= expect(create_unix_socket(runtime_dir / "wayland-1"),
		             "Wayland discovery test creates first socket");
		const auto single_display = howdy::native::find_wayland_display(runtime_dir);
		ok &= expect(single_display.has_value() && *single_display == "wayland-1",
		             "single Wayland socket is detected");

		ok &= expect(create_unix_socket(runtime_dir / "wayland-2"),
		             "Wayland discovery test creates second socket");
		ok &= expect(!howdy::native::find_wayland_display(runtime_dir).has_value(),
		             "multiple Wayland sockets are treated as ambiguous");

		std::filesystem::remove_all(runtime_dir);
	}

	unsetenv("XDG_RUNTIME_DIR");
	unsetenv("DBUS_SESSION_BUS_ADDRESS");
	unsetenv("WAYLAND_SOCKET");

	return ok ? 0 : 1;
}
