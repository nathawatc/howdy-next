#pragma once

#include "support/atomic_files.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace howdy::tools {

	[[nodiscard]] inline auto WriteOutputAtomically(const std::filesystem::path &output_path,
	                                                std::string_view             content)
	    -> std::optional<std::string> {
		const auto result = howdy::native::WriteAtomicFile(output_path, content);
		if (howdy::native::AtomicFileCommitIsDurable(result)) {
			return std::nullopt;
		}
		if (howdy::native::AtomicFileMayHaveCommitted(result)) {
			return "cannot durably replace output " + output_path.string() +
			       "; output was replaced, but its parent directory could not be synchronized";
		}
		return "cannot atomically write output " + output_path.string();
	}

}  // namespace howdy::tools
