#pragma once

#include "prompt/workaround.hpp"

#include <span>
#include <string_view>

namespace howdy::pam {

	inline constexpr std::string_view kWorkaroundOptionPrefix = "workaround=";
	inline constexpr Workaround       kDefaultWorkaround      = Workaround::kOff;

	struct WorkaroundDescriptor {
		std::string_view value;
		Workaround       workaround;
		std::string_view summary;
	};

	[[nodiscard]] auto WorkaroundCatalog() -> std::span<const WorkaroundDescriptor>;
	[[nodiscard]] auto FindWorkaround(std::string_view value) -> const WorkaroundDescriptor *;

}  // namespace howdy::pam
