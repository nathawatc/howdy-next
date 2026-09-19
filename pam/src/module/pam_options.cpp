#include "module/pam_options.hpp"

#include "module/pam_option_catalog.hpp"
#include "prompt/workaround.hpp"

#include <cstddef>
#include <span>
#include <string_view>

namespace howdy::pam {

	namespace {

		auto FindWorkaroundArgument(PamModuleArguments arguments) -> std::string_view {
			if (arguments.argv == nullptr || arguments.argc <= 0) {
				return {};
			}

			const auto raw_arguments =
			    std::span(arguments.argv, static_cast<std::size_t>(arguments.argc));
			for (const char *raw_argument : raw_arguments) {
				if (raw_argument == nullptr) {
					continue;
				}
				const std::string_view argument(raw_argument);
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
