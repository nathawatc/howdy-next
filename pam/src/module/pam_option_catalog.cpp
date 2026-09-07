#include "module/pam_option_catalog.hpp"

#include <array>

namespace howdy::pam {

	namespace {

		constexpr std::array<WorkaroundDescriptor, 3> kWorkaroundCatalog = {{
		    {
		        .value      = "input",
		        .workaround = Workaround::kInput,
		        .summary    = "Submit the active hidden-input prompt by injecting one Enter key.",
		    },
		    {
		        .value      = "native",
		        .workaround = Workaround::kNative,
		        .summary =
		            "Use native PAM conversation handling where a usable terminal is available.",
		    },
		    {
		        .value      = "native-input",
		        .workaround = Workaround::kNativeInput,
		        .summary    = "Try native prompt handling, then fall back to input injection.",
		    },
		}};

	}  // namespace

	auto WorkaroundCatalog() -> std::span<const WorkaroundDescriptor> {
		return kWorkaroundCatalog;
	}

	auto FindWorkaround(std::string_view value) -> const WorkaroundDescriptor * {
		for (const auto &descriptor : kWorkaroundCatalog) {
			if (descriptor.value == value) {
				return &descriptor;
			}
		}
		return nullptr;
	}

}  // namespace howdy::pam
