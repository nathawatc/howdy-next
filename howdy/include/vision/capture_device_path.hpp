#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include <sys/stat.h>

namespace howdy::native {
	inline constexpr auto kNoCaptureDevice = "none";

	inline constexpr std::array<std::string_view, 4> kAcceptedCaptureDevicePatterns = {
	    kNoCaptureDevice,
	    "/dev/video*",
	    "/dev/v4l/by-path/*",
	    "/dev/v4l/by-id/*",
	};

	namespace detail {

		inline auto is_allowed_capture_device_path(std::string_view             device_path,
		                                           const std::filesystem::path &root) -> bool {
			if (device_path.empty() || device_path == kNoCaptureDevice) {
				return true;
			}

			const auto dev_dir =
			    root.empty() ? std::filesystem::path("/dev") : (root / "dev").lexically_normal();
			const auto by_path_dir = root.empty() ? std::filesystem::path("/dev/v4l/by-path")
			                                      : (root / "dev/v4l/by-path").lexically_normal();
			const auto by_id_dir   = root.empty() ? std::filesystem::path("/dev/v4l/by-id")
			                                      : (root / "dev/v4l/by-id").lexically_normal();

			const auto is_direct_video_device_path =
			    [&](const std::filesystem::path &path) -> bool {
				const auto filename = path.filename().string();
				return path.parent_path() == dev_dir && filename.starts_with("video");
			};
			const auto is_v4l_persistent_device_path =
			    [&](const std::filesystem::path &path) -> bool {
				const auto parent   = path.parent_path();
				const auto filename = path.filename().string();
				return (parent == by_path_dir || parent == by_id_dir) && !filename.empty() &&
				       filename != "." && filename != "..";
			};

			const std::filesystem::path path{std::string(device_path)};
			if (!is_direct_video_device_path(path) && !is_v4l_persistent_device_path(path)) {
				return false;
			}

			std::error_code ec;
			const auto      entry_status = std::filesystem::symlink_status(path, ec);
			if (ec) {
				return ec == std::errc::no_such_file_or_directory;
			}
			if (entry_status.type() == std::filesystem::file_type::not_found) {
				return true;
			}

			const auto resolved_path = std::filesystem::canonical(path, ec);
			if (ec) {
				return false;
			}
			if (!is_direct_video_device_path(resolved_path)) {
				return false;
			}

			struct stat stat_{};
			return stat(resolved_path.c_str(), &stat_) == 0 && S_ISCHR(stat_.st_mode);
		}

	}  // namespace detail

	inline auto is_allowed_capture_device_path(std::string_view device_path) -> bool {
		return detail::is_allowed_capture_device_path(device_path, {});
	}

}  // namespace howdy::native
