#include "config/config_template.hpp"
#include "support/atomic_files.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

	auto write_output_atomically(const std::filesystem::path &output_path, std::string_view content)
	    -> std::optional<std::string> {
		const auto result = howdy::native::write_atomic_file(output_path, content);
		if (howdy::native::atomic_file_commit_is_durable(result)) {
			return std::nullopt;
		}
		if (howdy::native::atomic_file_may_have_committed(result)) {
			return "cannot durably replace output " + output_path.string() +
			       "; output was replaced, but its parent directory could not be synchronized";
		}
		return "cannot atomically write output " + output_path.string();
	}

	void print_usage() {
		std::cerr << "Usage: howdy_config_generator --output <path>\n";
	}

}  // namespace

auto main(int argc, char **argv) -> int {
	if (argc != 3 || std::string_view(argv[1]) != "--output" || std::string_view(argv[2]).empty()) {
		print_usage();
		return 2;
	}

	const auto rendered = howdy::native::config_template::render_default_config(
	    howdy::native::config_schema::runtime_config_options());
	if (!rendered.ok) {
		std::cerr << "howdy_config_generator: rendering failed: " << rendered.error << '\n';
		return 1;
	}

	const std::filesystem::path output_path(argv[2]);
	if (const auto error = write_output_atomically(output_path, rendered.content)) {
		std::cerr << "howdy_config_generator: " << *error << '\n';
		return 1;
	}
	return 0;
}
