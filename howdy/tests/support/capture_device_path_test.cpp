#include "support/capture_device_path.hpp"
#include "test_support.hpp"

#include <filesystem>

namespace {

	using howdy::test::Expect;
	using howdy::test::WriteFile;

}  // namespace

auto main() -> int {
	namespace fs = std::filesystem;

	bool ok = true;

	ok &= Expect(howdy::native::IsAllowedCaptureDevicePath("/dev/video999999"),
	             "missing /dev/video-style path is accepted by current policy");
	ok &= Expect(howdy::native::IsAllowedCaptureDevicePath("/dev/v4l/by-path/missing-camera"),
	             "missing /dev/v4l/by-path path is accepted by current policy");
	ok &= Expect(howdy::native::IsAllowedCaptureDevicePath("/dev/v4l/by-id/missing-camera"),
	             "missing /dev/v4l/by-id path is accepted by current policy");
	ok &= Expect(howdy::native::IsAllowedCaptureDevicePath("none"), "none sentinel is accepted");
	ok &= Expect(howdy::native::IsAllowedCaptureDevicePath(""),
	             "empty helper input remains accepted because config validation rejects empty");
	ok &= Expect(!howdy::native::IsAllowedCaptureDevicePath("0"),
	             "plain numeric device id is rejected");
	ok &= Expect(!howdy::native::IsAllowedCaptureDevicePath("dev/video0"),
	             "relative device path is rejected");
	ok &= Expect(!howdy::native::IsAllowedCaptureDevicePath("../dev/video0"),
	             "relative traversal device path is rejected");
	ok &= Expect(!howdy::native::IsAllowedCaptureDevicePath("/dev/v4l/by-path/../../null"),
	             "traversal from the allowed by-path namespace is rejected");
	ok &= Expect(!howdy::native::IsAllowedCaptureDevicePath("/dev/v4l/by-id/../../null"),
	             "traversal from the allowed by-id namespace is rejected");
	ok &= Expect(!howdy::native::IsAllowedCaptureDevicePath("/dev/video0/../null"),
	             "nested traversal from the direct video namespace is rejected");
	ok &= Expect(!howdy::native::IsAllowedCaptureDevicePath("/dev/v4l/by-path/.."),
	             "parent entry in the allowed by-path namespace is rejected");
	ok &= Expect(!howdy::native::IsAllowedCaptureDevicePath("/dev/v4l/by-id/.."),
	             "parent entry in the allowed by-id namespace is rejected");
	ok &= Expect(!howdy::native::IsAllowedCaptureDevicePath("/dev/v4l/by-path/."),
	             "self entry in the allowed by-path namespace is rejected");
	ok &= Expect(!howdy::native::IsAllowedCaptureDevicePath("/dev/v4l/by-id/."),
	             "self entry in the allowed by-id namespace is rejected");
	ok &= Expect(!howdy::native::IsAllowedCaptureDevicePath("/tmp/camera"),
	             "temporary camera path is rejected");
	ok &= Expect(!howdy::native::IsAllowedCaptureDevicePath("/dev/null"),
	             "path outside allowed video roots is rejected");

	std::error_code ec;
	const auto      temp_root = fs::current_path() / "howdy-capture-device-path-test";
	fs::remove_all(temp_root, ec);
	fs::create_directories(temp_root / "dev/v4l/by-id", ec);
	fs::create_directories(temp_root / "dev/v4l/by-path", ec);
	ok &= Expect(!ec, "create temp device tree");

	const auto fake_regular = temp_root / "dev" / "video-test";
	ok &= Expect(WriteFile(fake_regular, "not a character device"), "write fake regular device");
	ok &=
	    Expect(!howdy::native::detail::IsAllowedCaptureDevicePath(fake_regular.string(), temp_root),
	           "regular file in direct video namespace is rejected");

	const auto missing_by_id = temp_root / "dev" / "v4l" / "by-id" / "truly-missing-camera";
	ok &=
	    Expect(howdy::native::detail::IsAllowedCaptureDevicePath(missing_by_id.string(), temp_root),
	           "truly missing by-id path under temp root is accepted");

	const auto missing_by_path = temp_root / "dev" / "v4l" / "by-path" / "truly-missing-camera";
	ok &= Expect(
	    howdy::native::detail::IsAllowedCaptureDevicePath(missing_by_path.string(), temp_root),
	    "truly missing by-path path under temp root is accepted");

	const auto dangling_by_id = temp_root / "dev" / "v4l" / "by-id" / "dangling-camera";
	fs::create_symlink("../../video9999", dangling_by_id, ec);
	ok &= Expect(!ec, "create dangling by-id symlink");
	ok &= Expect(
	    !howdy::native::detail::IsAllowedCaptureDevicePath(dangling_by_id.string(), temp_root),
	    "dangling by-id symlink is rejected");

	const auto dangling_by_path = temp_root / "dev" / "v4l" / "by-path" / "dangling-camera";
	fs::create_symlink("../../video9999", dangling_by_path, ec);
	ok &= Expect(!ec, "create dangling by-path symlink");
	ok &= Expect(
	    !howdy::native::detail::IsAllowedCaptureDevicePath(dangling_by_path.string(), temp_root),
	    "dangling by-path symlink is rejected");

	const auto fake_video0 = temp_root / "dev" / "video0";
	ok &= Expect(WriteFile(fake_video0, "fake video device content"),
	             "write fake regular video device");

	const auto regular_target_by_id = temp_root / "dev" / "v4l" / "by-id" / "regular-target-camera";
	fs::create_symlink("../../video0", regular_target_by_id, ec);
	ok &= Expect(!ec, "create by-id symlink to regular file target");
	ok &= Expect(!howdy::native::detail::IsAllowedCaptureDevicePath(regular_target_by_id.string(),
	                                                                temp_root),
	             "by-id symlink pointing to non-character-device target is rejected");

	const auto outside_target_by_id = temp_root / "dev" / "v4l" / "by-id" / "outside-target-camera";
	fs::create_symlink("/dev/null", outside_target_by_id, ec);
	ok &= Expect(!ec, "create by-id symlink to outside target");
	ok &= Expect(!howdy::native::detail::IsAllowedCaptureDevicePath(outside_target_by_id.string(),
	                                                                temp_root),
	             "by-id symlink pointing to target outside allowed video namespace is rejected");

	fs::remove_all(temp_root, ec);

	return ok ? 0 : 1;
}
