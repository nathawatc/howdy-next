#pragma once

#include <sys/stat.h>

#include <string>
#include <string_view>

namespace howdy::native {

inline auto is_allowed_capture_device_path(std::string_view device_path)
    -> bool {
  if (device_path.empty() || device_path == "none") {
    return true;
  }

  const std::string value(device_path);
  if (value.rfind("/dev/video", 0) != 0 &&
      value.rfind("/dev/v4l/by-path/", 0) != 0) {
    return false;
  }

  struct stat stat_ {};
  if (stat(value.c_str(), &stat_) != 0) {
    return true;
  }

  return S_ISCHR(stat_.st_mode);
}

}  // namespace howdy::native
