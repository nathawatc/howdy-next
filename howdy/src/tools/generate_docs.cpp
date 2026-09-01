#include "app/command_catalog.hpp"
#include "docs/man_reference.hpp"
#include "module/pam_option_catalog.hpp"
#include "support/atomic_files.hpp"

#include <array>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>

namespace {

	namespace fs = std::filesystem;

	struct Fragment {
		std::string_view filename;
		std::string_view content;
	};

	auto write_fragment(const fs::path &path, std::string_view content)
	    -> std::optional<std::string> {
		const auto result = howdy::native::write_atomic_file(path, content);
		if (howdy::native::atomic_file_commit_is_durable(result)) {
			return std::nullopt;
		}
		if (howdy::native::atomic_file_may_have_committed(result)) {
			return "cannot durably replace output " + path.string() +
			       "; output was replaced, but its parent directory could not be synchronized";
		}
		return "cannot atomically write output " + path.string();
	}

	void print_usage() {
		std::cerr << "Usage: howdy_docs_generator --output-dir <directory>\n";
	}

}  // namespace

auto main(int argc, char **argv) -> int {
	if (argc != 3 || std::string_view(argv[1]) != "--output-dir" ||
	    std::string_view(argv[2]).empty()) {
		print_usage();
		return 2;
	}

	if (const auto error =
	        howdy::native::validate_command_catalog(howdy::native::command_catalog(), true)) {
		std::cerr << "howdy_docs_generator: command catalog validation failed: " << *error << '\n';
		return 1;
	}
	if (const auto error = howdy::native::validate_global_option_catalog(
	        howdy::native::global_option_catalog(), true)) {
		std::cerr << "howdy_docs_generator: global option catalog validation failed: " << *error
		          << '\n';
		return 1;
	}
	if (const auto error = howdy::native::config_schema::validate_options(
	        howdy::native::config_schema::runtime_config_options())) {
		std::cerr << "howdy_docs_generator: config schema validation failed: " << *error << '\n';
		return 1;
	}

	const auto command_reference =
	    howdy::docs::render_command_reference(howdy::native::command_catalog());
	const auto option_reference =
	    howdy::docs::render_global_option_reference(howdy::native::global_option_catalog());
	const auto workaround_reference = howdy::docs::render_workaround_reference(
	    howdy::pam::workaround_catalog(), howdy::pam::kDefaultWorkaround);
	const auto config_reference = howdy::docs::render_config_option_reference(
	    howdy::native::config_schema::runtime_config_options());
	if (!command_reference.ok()) {
		std::cerr << "howdy_docs_generator: rendering howdy-commands.roff failed: "
		          << command_reference.error << '\n';
		return 1;
	}
	if (!option_reference.ok()) {
		std::cerr << "howdy_docs_generator: rendering howdy-options.roff failed: "
		          << option_reference.error << '\n';
		return 1;
	}
	if (!workaround_reference.ok()) {
		std::cerr << "howdy_docs_generator: rendering pam-workarounds.roff failed: "
		          << workaround_reference.error << '\n';
		return 1;
	}
	if (!config_reference.ok()) {
		std::cerr << "howdy_docs_generator: rendering howdy-ini-options.roff failed: "
		          << config_reference.error << '\n';
		return 1;
	}

	const std::array fragments{
	    Fragment{.filename = "howdy-commands.roff", .content = command_reference.output},
	    Fragment{.filename = "howdy-options.roff", .content = option_reference.output},
	    Fragment{.filename = "pam-workarounds.roff", .content = workaround_reference.output},
	    Fragment{.filename = "howdy-ini-options.roff", .content = config_reference.output},
	};
	const fs::path output_dir(argv[2]);
	for (const auto &fragment : fragments) {
		const auto path = output_dir / fragment.filename;
		if (const auto error = write_fragment(path, fragment.content)) {
			std::cerr << "howdy_docs_generator: " << *error << '\n';
			return 1;
		}
	}
	return 0;
}
