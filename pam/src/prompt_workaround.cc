#include "prompt_workaround.hh"

auto should_ask_for_password(bool ask_auth_tok, Workaround workaround) -> bool {
  return ask_auth_tok && workaround != Workaround::Off;
}

auto plan_prompt_stop(bool password_prompt_active, bool prompt_ready,
                      Workaround workaround) -> PromptStopPlan {
  PromptStopPlan plan;
  if (!password_prompt_active) {
    return plan;
  }

  plan.stop_prompt = true;
  if (prompt_ready) {
    return plan;
  }

  if (workaround == Workaround::Native) {
    plan.force_cancel = true;
    return plan;
  }

  if (workaround == Workaround::Input) {
    plan.send_enter = true;
  }

  return plan;
}
