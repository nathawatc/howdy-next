#include "module/pam_options.hpp"

#include "module/pam_option_catalog.hpp"

namespace howdy::pam {

	namespace {

		auto find_workaround_argument(PamModuleArguments arguments) -> std::string_view {
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

		auto map_workaround_value(std::string_view value) -> Workaround {
			if (const auto *descriptor = find_workaround(value); descriptor != nullptr) {
				return descriptor->workaround;
			}
			return kDefaultWorkaround;
		}

	}  // namespace

	auto parse_pam_options(PamModuleArguments arguments) -> PamOptions {
		return {.workaround = map_workaround_value(find_workaround_argument(arguments))};
	}

}  // namespace howdy::pam
