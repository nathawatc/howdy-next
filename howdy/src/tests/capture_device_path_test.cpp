#include "common/capture_device_path.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace {

	using howdy::test::expect;

	auto write_file(const std::filesystem::path &path, const std::string &content) -> bool {
		std::ofstream out(path);
		if (!out.is_open()) {
			return false;
		}
		out << content;
		return out.good();
	}

}  // namespace

auto main() -> int {
	namespace fs = std::filesystem;

	bool ok = true;

	ok &= expect(howdy::native::is_allowed_capture_device_path("/dev/video999999"),
	             "missing /dev/video-style path is accepted by current policy");
	ok &= expect(howdy::native::is_allowed_capture_device_path("/dev/v4l/by-path/missing-camera"),
	             "missing /dev/v4l/by-path path is accepted by current policy");
	ok &=
	    expect(howdy::native::is_allowed_capture_device_path("none"), "none sentinel is accepted");
	ok &= expect(howdy::native::is_allowed_capture_device_path(""),
	             "empty helper input remains accepted because config validation rejects empty");
	ok &= expect(!howdy::native::is_allowed_capture_device_path("0"),
	             "plain numeric device id is rejected");
	ok &= expect(!howdy::native::is_allowed_capture_device_path("dev/video0"),
	             "relative device path is rejected");
	ok &= expect(!howdy::native::is_allowed_capture_device_path("../dev/video0"),
	             "relative traversal device path is rejected");
	ok &= expect(!howdy::native::is_allowed_capture_device_path("/tmp/camera"),
	             "temporary camera path is rejected");
	ok &= expect(!howdy::native::is_allowed_capture_device_path("/dev/null"),
	             "path outside allowed video roots is rejected");

	const auto      temp_root = fs::current_path() / "howdy-capture-device-path-test";
	std::error_code ec;
	fs::remove_all(temp_root, ec);
	fs::create_directories(temp_root / "dev", ec);
	ok &= expect(!ec, "create temp device tree");

	const auto fake_regular = temp_root / "dev" / "video-test";
	ok &= expect(write_file(fake_regular, "not a character device"), "write fake regular device");
	ok &= expect(!howdy::native::is_allowed_capture_device_path(fake_regular.string()),
	             "regular file outside allowed roots is rejected");

	fs::remove_all(temp_root, ec);

	return ok ? 0 : 1;
}
