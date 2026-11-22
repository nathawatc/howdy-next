#include "atomic_output.hpp"
#include "config/config_schema.hpp"
#include "config/config_template.hpp"

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <span>
#include <string_view>

namespace {

	void PrintUsage() {
		std::cerr << "Usage: howdy_config_generator --output <path>\n";
	}

}  // namespace

auto main(int argc, char **argv) -> int {
	const auto arguments =
	    std::span(argv, argc > 0 ? static_cast<std::size_t>(argc) : std::size_t{0});
	if (arguments.size() != 3 || std::string_view(arguments.subspan(1, 1).front()) != "--output" ||
	    std::string_view(arguments.subspan(2, 1).front()).empty()) {
		PrintUsage();
		return 2;
	}

	const auto rendered = howdy::native::config_template::RenderDefaultConfig(
	    howdy::native::config_schema::RuntimeConfigOptions());
	if (!rendered.ok) {
		std::cerr << "howdy_config_generator: rendering failed: " << rendered.error << '\n';
		return 1;
	}

	const std::filesystem::path output_path(arguments.subspan(2, 1).front());
	if (const auto error = howdy::tools::WriteOutputAtomically(output_path, rendered.content)) {
		std::cerr << "howdy_config_generator: " << *error << '\n';
		return 1;
	}
	return 0;
}
