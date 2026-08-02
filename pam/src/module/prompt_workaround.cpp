#include "module/prompt_workaround.hpp"

auto should_ask_for_password(bool ask_auth_tok, Workaround workaround, bool auth_token_available)
    -> bool {
	return ask_auth_tok && !auth_token_available && workaround != Workaround::Off;
}
