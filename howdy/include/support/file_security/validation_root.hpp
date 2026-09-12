#pragma once

#include <filesystem>
#include <optional>

namespace howdy::native::file_security_internal {

	// Empty selects the production policy: validate absolute paths from /.
	// A nonempty root is for callers that own a controlled fixture tree. The
	// root itself is checked; only ancestors above it are outside the boundary.
	struct ValidationRoot {
		std::filesystem::path path;
	};

	inline auto RelativeTarget(const ValidationRoot &root, const std::filesystem::path &target)
	    -> std::optional<std::filesystem::path> {
		if (!root.path.is_absolute() || !target.is_absolute() ||
		    root.path.native().contains('\0') || target.native().contains('\0')) {
			return std::nullopt;
		}
		for (const auto &path : {root.path, target}) {
			for (const auto &component : path) {
				if (component == "..") {
					return std::nullopt;
				}
			}
		}
		auto relative = target.lexically_relative(root.path);
		if (relative.empty()) {
			return std::nullopt;
		}
		for (const auto &component : relative) {
			if (component == "..") {
				return std::nullopt;
			}
		}
		return relative;
	}

}  // namespace howdy::native::file_security_internal
