#include "module/pam_option_catalog.hpp"

#include <array>

namespace howdy::pam {

	namespace {

		constexpr std::array<WorkaroundDescriptor, 3> kWorkaroundCatalog = {{
		    {
		        .value      = "input",
		        .workaround = Workaround::kInput,
		        .summary    = "Inject one Enter key after a hidden-input prompt is observed.",
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

	auto workaround_catalog() -> std::span<const WorkaroundDescriptor> {
		return kWorkaroundCatalog;
	}

	auto find_workaround(std::string_view value) -> const WorkaroundDescriptor * {
		for (const auto &descriptor : kWorkaroundCatalog) {
			if (descriptor.value == value) {
				return &descriptor;
			}
		}
		return nullptr;
	}

}  // namespace howdy::pam
