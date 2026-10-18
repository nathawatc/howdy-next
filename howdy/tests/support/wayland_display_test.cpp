#include "support/wayland_display.hpp"
#include "test_support.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <stdlib.h>

#include <sys/stat.h>

namespace {

	using howdy::test::Expect;

	auto ExpectSocketNode(const std::filesystem::path &path, std::string_view description) -> bool {
		if (mknod(path.c_str(), S_IFSOCK | 0600, 0) == 0) {
			return true;
		}

		const int error = errno;
		return Expect(false, std::string(description) + ": errno " + std::to_string(error) + " (" +
		                         std::strerror(error) + ")");
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	auto        runtime_template = std::to_array("/tmp/howdy-wayland-test-XXXXXX");
	char const *runtime_path     = mkdtemp(runtime_template.data());
	ok &= Expect(runtime_path != nullptr, "Wayland display test creates runtime directory");
	if (runtime_path != nullptr) {
		const std::filesystem::path runtime_dir(runtime_path);
		ok &= Expect(!howdy::native::FindWaylandDisplay(runtime_dir).has_value(),
		             "runtime directory without Wayland socket has no display");

		std::ofstream(runtime_dir / "wayland-0.lock") << "lock";
		ok &= ExpectSocketNode(runtime_dir / "wayland-1",
		                       "Wayland display test creates first socket");
		const auto single_display = howdy::native::FindWaylandDisplay(runtime_dir);
		ok &= Expect(single_display.has_value() && *single_display == "wayland-1",
		             "single Wayland socket is detected");

		ok &= ExpectSocketNode(runtime_dir / "wayland-2",
		                       "Wayland display test creates second socket");
		ok &= Expect(!howdy::native::FindWaylandDisplay(runtime_dir).has_value(),
		             "multiple Wayland sockets are treated as ambiguous");

		std::filesystem::remove_all(runtime_dir);
	}

	return ok ? 0 : 1;
}
