#ifndef HOWDY_PAM_PROMPT_WORKAROUND_HH
#define HOWDY_PAM_PROMPT_WORKAROUND_HH

#include "module/main.hpp"

struct PromptStopPlan {
	bool stop_prompt  = false;
	bool abort_prompt = false;
	bool send_enter   = false;
};

auto should_ask_for_password(bool ask_auth_tok, Workaround workaround, bool auth_token_available)
    -> bool;
auto plan_prompt_stop(bool password_prompt_active, bool prompt_ready, Workaround workaround)
    -> PromptStopPlan;

#endif  // HOWDY_PAM_PROMPT_WORKAROUND_HH
