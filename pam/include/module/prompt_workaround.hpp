#ifndef HOWDY_PAM_PROMPT_WORKAROUND_HH
#define HOWDY_PAM_PROMPT_WORKAROUND_HH

#include "module/main.hpp"

auto should_ask_for_password(bool ask_auth_tok, Workaround workaround, bool auth_token_available)
    -> bool;

#endif  // HOWDY_PAM_PROMPT_WORKAROUND_HH
