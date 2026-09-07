#pragma once

#include <cstdint>

namespace howdy::pam {

	enum class Workaround : std::uint8_t {
		kOff,
		kInput,
		kNative,
		kNativeInput,
	};

	__attribute__((visibility("hidden"))) auto
	ShouldAskForPassword(bool ask_auth_tok, Workaround workaround, bool auth_token_available)
	    -> bool;

}  // namespace howdy::pam
