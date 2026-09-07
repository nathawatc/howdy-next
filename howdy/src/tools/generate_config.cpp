#include "config/config_template.hpp"
#include "support/atomic_files.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

	auto WriteOutputAtomically(const std::filesystem::path &output_path, std::string_view content)
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

	void PrintUsage() {
		std::cerr << "Usage: howdy_config_generator --output <path>\n";
	}

}  // namespace

auto main(int argc, char **argv) -> int {
	if (argc != 3 || std::string_view(argv[1]) != "--output" || std::string_view(argv[2]).empty()) {
		PrintUsage();
		return 2;
	}

	const auto rendered = howdy::native::config_template::RenderDefaultConfig(
	    howdy::native::config_schema::RuntimeConfigOptions());
	if (!rendered.ok) {
		std::cerr << "howdy_config_generator: rendering failed: " << rendered.error << '\n';
		return 1;
	}

	const std::filesystem::path output_path(argv[2]);
	if (const auto error = WriteOutputAtomically(output_path, rendered.content)) {
		std::cerr << "howdy_config_generator: " << *error << '\n';
		return 1;
	}
	return 0;
}
