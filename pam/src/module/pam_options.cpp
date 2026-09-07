#include "module/pam_options.hpp"

#include "module/pam_option_catalog.hpp"

namespace howdy::pam {

	namespace {

		auto FindWorkaroundArgument(PamModuleArguments arguments) -> std::string_view {
			if (arguments.argv == nullptr) {
				return {};
			}

			for (int index = 0; index < arguments.argc; ++index) {
				if (arguments.argv[index] == nullptr) {
					continue;
				}
				const std::string_view argument(arguments.argv[index]);
				if (argument.starts_with(kWorkaroundOptionPrefix)) {
					return argument.substr(kWorkaroundOptionPrefix.size());
				}
			}
			return {};
		}

		auto MapWorkaroundValue(std::string_view value) -> Workaround {
			if (const auto *descriptor = FindWorkaround(value); descriptor != nullptr) {
				return descriptor->workaround;
			}
			return kDefaultWorkaround;
		}

	}  // namespace

	auto ParsePamOptions(PamModuleArguments arguments) -> PamOptions {
		return {.workaround = MapWorkaroundValue(FindWorkaroundArgument(arguments))};
	}

}  // namespace howdy::pam
