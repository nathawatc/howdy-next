#include "module/pam_options.hpp"

#include <array>
#include <string_view>

namespace howdy::pam {

	namespace {

		struct WorkaroundMapping {
			std::string_view value;
			Workaround       workaround;
		};

		constexpr std::array kWorkaroundMappings{
		    WorkaroundMapping{.value = "input", .workaround = Workaround::kInput},
		    WorkaroundMapping{.value = "native", .workaround = Workaround::kNative},
		    WorkaroundMapping{.value = "native-input", .workaround = Workaround::kNativeInput},
		};

		constexpr std::string_view kWorkaroundPrefix = "workaround=";

		auto find_workaround_argument(PamModuleArguments arguments) -> std::string_view {
			if (arguments.argv == nullptr) {
				return {};
			}

			for (int index = 0; index < arguments.argc; ++index) {
				if (arguments.argv[index] == nullptr) {
					continue;
				}
				const std::string_view argument(arguments.argv[index]);
				if (argument.starts_with(kWorkaroundPrefix)) {
					return argument.substr(kWorkaroundPrefix.size());
				}
			}
			return {};
		}

		auto map_workaround_value(std::string_view value) -> Workaround {
			for (const auto &mapping : kWorkaroundMappings) {
				if (mapping.value == value) {
					return mapping.workaround;
				}
			}
			return Workaround::kOff;
		}

	}  // namespace

	auto parse_pam_options(PamModuleArguments arguments) -> PamOptions {
		return {.workaround = map_workaround_value(find_workaround_argument(arguments))};
	}

}  // namespace howdy::pam
