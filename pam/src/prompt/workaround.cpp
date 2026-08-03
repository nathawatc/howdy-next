#include "prompt/workaround.hpp"

namespace howdy::pam {

	auto should_ask_for_password(bool ask_auth_tok, Workaround workaround,
	                             bool auth_token_available) -> bool {
		return ask_auth_tok && !auth_token_available && workaround != Workaround::kOff;
	}

}  // namespace howdy::pam
