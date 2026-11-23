#include "module/pam_option_catalog.hpp"

#include "prompt/workaround.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

namespace howdy::pam {

	namespace {

		constexpr std::array<WorkaroundDescriptor, 3> kWorkaroundCatalog = {
		    {
		        {
		            .value      = "input",
		            .workaround = Workaround::kInput,
		            .summary = "Submit the active hidden-input prompt by injecting one Enter key.",
		        },
		        {
		            .value      = "native",
		            .workaround = Workaround::kNative,
		            .summary    = "Use native PAM conversation handling where a usable terminal is "
		                          "available.",
		        },
		        {
		            .value      = "native-input",
		            .workaround = Workaround::kNativeInput,
		            .summary    = "Try native prompt handling, then fall back to input injection.",
		        },
		    },
		};

	}  // namespace

	auto WorkaroundCatalog() -> std::span<const WorkaroundDescriptor> {
		return kWorkaroundCatalog;
	}

	auto FindWorkaround(std::string_view value) -> const WorkaroundDescriptor * {
		const auto *const it =
		    std::ranges::find(kWorkaroundCatalog, value, &WorkaroundDescriptor::value);
		return it == kWorkaroundCatalog.end() ? nullptr : &*it;
	}

}  // namespace howdy::pam
