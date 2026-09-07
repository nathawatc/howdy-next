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

	auto WriteFragment(const fs::path &path, std::string_view content)
	    -> std::optional<std::string> {
		const auto result = howdy::native::WriteAtomicFile(path, content);
		if (howdy::native::AtomicFileCommitIsDurable(result)) {
			return std::nullopt;
		}
		if (howdy::native::AtomicFileMayHaveCommitted(result)) {
			return "cannot durably replace output " + path.string() +
			       "; output was replaced, but its parent directory could not be synchronized";
		}
		return "cannot atomically write output " + path.string();
	}

	void PrintUsage() {
		std::cerr << "Usage: howdy_docs_generator --output-dir <directory>\n";
	}

}  // namespace

auto main(int argc, char **argv) -> int {
	if (argc != 3 || std::string_view(argv[1]) != "--output-dir" ||
	    std::string_view(argv[2]).empty()) {
		PrintUsage();
		return 2;
	}

	if (const auto error =
	        howdy::native::ValidateCommandCatalog(howdy::native::CommandCatalog(), true)) {
		std::cerr << "howdy_docs_generator: command catalog validation failed: " << *error << '\n';
		return 1;
	}
	if (const auto error = howdy::native::ValidateGlobalOptionCatalog(
	        howdy::native::GlobalOptionCatalog(), true)) {
		std::cerr << "howdy_docs_generator: global option catalog validation failed: " << *error
		          << '\n';
		return 1;
	}
	if (const auto error = howdy::native::config_schema::ValidateOptions(
	        howdy::native::config_schema::RuntimeConfigOptions())) {
		std::cerr << "howdy_docs_generator: config schema validation failed: " << *error << '\n';
		return 1;
	}

	const auto command_reference =
	    howdy::docs::RenderCommandReference(howdy::native::CommandCatalog());
	const auto option_reference =
	    howdy::docs::RenderGlobalOptionReference(howdy::native::GlobalOptionCatalog());
	const auto workaround_reference = howdy::docs::RenderWorkaroundReference(
	    howdy::pam::WorkaroundCatalog(), howdy::pam::kDefaultWorkaround);
	const auto config_reference = howdy::docs::RenderConfigOptionReference(
	    howdy::native::config_schema::RuntimeConfigOptions());
	if (!command_reference.Ok()) {
		std::cerr << "howdy_docs_generator: rendering howdy-commands.roff failed: "
		          << command_reference.error << '\n';
		return 1;
	}
	if (!option_reference.Ok()) {
		std::cerr << "howdy_docs_generator: rendering howdy-options.roff failed: "
		          << option_reference.error << '\n';
		return 1;
	}
	if (!workaround_reference.Ok()) {
		std::cerr << "howdy_docs_generator: rendering pam-workarounds.roff failed: "
		          << workaround_reference.error << '\n';
		return 1;
	}
	if (!config_reference.Ok()) {
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
		if (const auto error = WriteFragment(path, fragment.content)) {
			std::cerr << "howdy_docs_generator: " << *error << '\n';
			return 1;
		}
	}
	return 0;
}
