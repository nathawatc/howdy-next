#include "app/command_catalog.hpp"
#include "atomic_output.hpp"
#include "config/config_schema.hpp"
#include "docs/man_reference.hpp"
#include "module/pam_option_catalog.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <span>
#include <string_view>

namespace {

	namespace fs = std::filesystem;

	struct Fragment {
		std::string_view          filename;
		howdy::docs::RenderResult rendered;
	};

	void PrintUsage() {
		std::cerr << "Usage: howdy_docs_generator --output-dir <directory>\n";
	}

}  // namespace

auto main(int argc, char **argv) -> int {
	const auto arguments =
	    std::span(argv, argc > 0 ? static_cast<std::size_t>(argc) : std::size_t{0});
	if (arguments.size() != 3 ||
	    std::string_view(arguments.subspan(1, 1).front()) != "--output-dir" ||
	    std::string_view(arguments.subspan(2, 1).front()).empty()) {
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

	const std::array fragments{
	    Fragment{
	        .filename = "howdy-commands.roff",
	        .rendered = howdy::docs::RenderCommandReference(howdy::native::CommandCatalog()),
	    },
	    Fragment{
	        .filename = "howdy-options.roff",
	        .rendered =
	            howdy::docs::RenderGlobalOptionReference(howdy::native::GlobalOptionCatalog()),
	    },
	    Fragment{
	        .filename = "pam-workarounds.roff",
	        .rendered = howdy::docs::RenderWorkaroundReference(howdy::pam::WorkaroundCatalog(),
			                                                   howdy::pam::kDefaultWorkaround),
	    },
	    Fragment{
	        .filename = "howdy-ini-options.roff",
	        .rendered = howdy::docs::RenderConfigOptionReference(
	            howdy::native::config_schema::RuntimeConfigOptions()),
	    },
	};
	for (const auto &fragment : fragments) {
		if (!howdy::docs::RenderSucceeded(fragment.rendered)) {
			std::cerr << "howdy_docs_generator: rendering " << fragment.filename
			          << " failed: " << fragment.rendered.error << '\n';
			return 1;
		}
	}

	const fs::path output_dir(arguments.subspan(2, 1).front());
	for (const auto &fragment : fragments) {
		const auto path = output_dir / fragment.filename;
		if (const auto error =
		        howdy::tools::WriteOutputAtomically(path, fragment.rendered.output)) {
			std::cerr << "howdy_docs_generator: " << *error << '\n';
			return 1;
		}
	}
	return 0;
}
