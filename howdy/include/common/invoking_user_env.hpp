#pragma once

#include "common/invoking_user.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace howdy::native {

inline void set_user_env_var(const char *name, const std::string &value) {
  if (value.empty()) {
    unsetenv(name);
    return;
  }
  setenv(name, value.c_str(), 1);
}

inline void reset_invoking_user_environment(const InvokingUser &invoking_user) {
  set_user_env_var("HOME", invoking_user.home);
  set_user_env_var("LOGNAME", invoking_user.name);
  set_user_env_var("USER", invoking_user.name);
  set_user_env_var("SHELL", invoking_user.shell);

  unsetenv("XDG_CONFIG_HOME");
  unsetenv("XDG_CACHE_HOME");
  unsetenv("XDG_DATA_HOME");
  unsetenv("XDG_STATE_HOME");
}

inline void reset_invoking_user_gui_environment(
    const InvokingUser &invoking_user) {
  reset_invoking_user_environment(invoking_user);
  unsetenv("XDG_RUNTIME_DIR");
  unsetenv("DBUS_SESSION_BUS_ADDRESS");

  const auto runtime_dir =
      std::filesystem::path("/run/user") / std::to_string(invoking_user.uid);
  std::error_code ec;
  if (std::filesystem::is_directory(runtime_dir, ec) && !ec) {
    set_user_env_var("XDG_RUNTIME_DIR", runtime_dir.string());

    const auto session_bus = runtime_dir / "bus";
    ec.clear();
    if (std::filesystem::exists(session_bus, ec) && !ec) {
      set_user_env_var("DBUS_SESSION_BUS_ADDRESS",
                       "unix:path=" + session_bus.string());
    }
  }

  if (std::getenv("XAUTHORITY") == nullptr && !invoking_user.home.empty()) {
    const auto xauthority =
        std::filesystem::path(invoking_user.home) / ".Xauthority";
    ec.clear();
    if (std::filesystem::is_regular_file(xauthority, ec) && !ec) {
      set_user_env_var("XAUTHORITY", xauthority.string());
    }
  }
}

}  // namespace howdy::native
