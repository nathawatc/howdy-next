#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace howdy::native {

	inline auto FindWaylandDisplay(const std::filesystem::path &runtime_dir)
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

}  // namespace howdy::native
