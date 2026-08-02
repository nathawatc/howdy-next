#pragma once

#include <cstdint>

namespace howdy::pam {

	enum class ConversationRestoreResult : std::uint8_t {
		kOriginalRestored,
		kFailClosedInstalled,
		kUnsafe,
	};

}  // namespace howdy::pam
