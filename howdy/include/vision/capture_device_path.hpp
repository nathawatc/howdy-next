#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include <sys/stat.h>

namespace howdy::native {

	inline auto is_allowed_capture_device_path(std::string_view device_path) -> bool {
		if (device_path.empty() || device_path == "none") {
			return true;
		}

		const auto is_direct_video_device_path = [](const std::filesystem::path &path) -> bool {
			const auto filename = path.filename().string();
			return path.parent_path() == "/dev" && filename.starts_with("video");
		};
		const auto is_v4l_by_path_device_path = [](const std::filesystem::path &path) -> bool {
			const auto filename = path.filename().string();
			return path.parent_path() == "/dev/v4l/by-path" && !filename.empty() &&
			       filename != "." && filename != "..";
		};

		const std::filesystem::path path{std::string(device_path)};
		if (!is_direct_video_device_path(path) && !is_v4l_by_path_device_path(path)) {
			return false;
		}

		std::error_code ec;
		const auto      resolved_path = std::filesystem::canonical(path, ec);
		if (ec) {
			return ec == std::errc::no_such_file_or_directory;
		}
		if (!is_direct_video_device_path(resolved_path)) {
			return false;
		}

		struct stat stat_{};
		return stat(resolved_path.c_str(), &stat_) == 0 && S_ISCHR(stat_.st_mode);
	}

}  // namespace howdy::native
