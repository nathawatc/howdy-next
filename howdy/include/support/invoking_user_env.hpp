#pragma once

#include "support/invoking_user.hpp"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
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

	inline auto find_wayland_display(const std::filesystem::path &runtime_dir)
	    -> std::optional<std::string> {
		std::optional<std::string>          display;
		std::error_code                     ec;
		std::filesystem::directory_iterator iterator(
		    runtime_dir, std::filesystem::directory_options::skip_permission_denied, ec);
		if (ec) {
			return std::nullopt;
		}

		for (const std::filesystem::directory_iterator end; iterator != end;
		     iterator.increment(ec)) {
			if (ec) {
				return std::nullopt;
			}

			const auto                 name      = iterator->path().filename().string();
			const std::string_view     name_view = name;
			constexpr std::string_view prefix    = "wayland-";
			if (!name_view.starts_with(prefix)) {
				continue;
			}
			const auto suffix = name_view.substr(prefix.size());
			if (suffix.empty() ||
			    suffix.find_first_not_of("0123456789") != std::string_view::npos) {
				continue;
			}

			ec.clear();
			if (iterator->symlink_status(ec).type() != std::filesystem::file_type::socket || ec) {
				ec.clear();
				continue;
			}

			if (display.has_value()) {
				return std::nullopt;
			}
			display = name;
		}

		return display;
	}

	inline void prepare_invoking_user_gui_environment(const InvokingUser &invoking_user) {
		unsetenv("WAYLAND_SOCKET");
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
				set_user_env_var("DBUS_SESSION_BUS_ADDRESS", "unix:path=" + session_bus.string());
			}

			const char *wayland_display = std::getenv("WAYLAND_DISPLAY");
			if (wayland_display == nullptr || wayland_display[0] == '\0') {
				if (const auto display = find_wayland_display(runtime_dir)) {
					set_user_env_var("WAYLAND_DISPLAY", *display);
				}
			}
		}

		if (std::getenv("XAUTHORITY") == nullptr && !invoking_user.home.empty()) {
			const auto xauthority = std::filesystem::path(invoking_user.home) / ".Xauthority";
			ec.clear();
			if (std::filesystem::is_regular_file(xauthority, ec) && !ec) {
				set_user_env_var("XAUTHORITY", xauthority.string());
			}
		}
	}

	inline void reset_invoking_user_gui_environment(const InvokingUser &invoking_user) {
		reset_invoking_user_environment(invoking_user);
		prepare_invoking_user_gui_environment(invoking_user);
	}

}  // namespace howdy::native
