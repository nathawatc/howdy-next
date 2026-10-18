#pragma once

#include "support/invoking_user.hpp"
#include "support/wayland_display.hpp"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

#include <unistd.h>

namespace howdy::native {
	inline constexpr auto kWaylandDisplayEnvironmentVariable = "WAYLAND_DISPLAY";
	inline constexpr auto kXdgRuntimeDirEnvironmentVariable  = "XDG_RUNTIME_DIR";

	inline void SetUserEnvVar(const char *name, const std::string &value) {
		if (value.empty()) {
			unsetenv(name);
			return;
		}
		setenv(name, value.c_str(), 1);
	}

	inline void ResetInvokingUserEnvironment(const InvokingUser &invoking_user) {
		SetUserEnvVar("HOME", invoking_user.home);
		SetUserEnvVar("LOGNAME", invoking_user.name);
		SetUserEnvVar("USER", invoking_user.name);
		SetUserEnvVar("SHELL", invoking_user.shell);

		unsetenv("XDG_CONFIG_HOME");
		unsetenv("XDG_CACHE_HOME");
		unsetenv("XDG_DATA_HOME");
		unsetenv("XDG_STATE_HOME");
	}

	inline void PrepareInvokingUserGuiEnvironment(const InvokingUser &invoking_user) {
		unsetenv("WAYLAND_SOCKET");
		unsetenv(kXdgRuntimeDirEnvironmentVariable);
		unsetenv("DBUS_SESSION_BUS_ADDRESS");

		const auto runtime_dir =
		    std::filesystem::path("/run/user") / std::to_string(invoking_user.uid);
		std::error_code ec;
		if (std::filesystem::is_directory(runtime_dir, ec) && !ec) {
			SetUserEnvVar(kXdgRuntimeDirEnvironmentVariable, runtime_dir.string());

			const auto session_bus = runtime_dir / "bus";
			ec.clear();
			if (std::filesystem::exists(session_bus, ec) && !ec) {
				SetUserEnvVar("DBUS_SESSION_BUS_ADDRESS", "unix:path=" + session_bus.string());
			}

			const char *wayland_display = std::getenv(kWaylandDisplayEnvironmentVariable);
			if (wayland_display == nullptr || *wayland_display == '\0') {
				if (const auto display = FindWaylandDisplay(runtime_dir)) {
					SetUserEnvVar(kWaylandDisplayEnvironmentVariable, *display);
				}
			}
		}

		if (std::getenv("XAUTHORITY") == nullptr && !invoking_user.home.empty()) {
			const auto xauthority = std::filesystem::path(invoking_user.home) / ".Xauthority";
			ec.clear();
			if (std::filesystem::is_regular_file(xauthority, ec) && !ec) {
				SetUserEnvVar("XAUTHORITY", xauthority.string());
			}
		}
	}

	inline void ResetInvokingUserGuiEnvironment(const InvokingUser &invoking_user) {
		ResetInvokingUserEnvironment(invoking_user);
		PrepareInvokingUserGuiEnvironment(invoking_user);
	}

}  // namespace howdy::native
