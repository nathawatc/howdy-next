#include "prompt_workaround.hpp"

#include <iostream>
#include <string>

namespace {

auto expect(bool condition, const std::string &message) -> bool {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    return false;
  }
  return true;
}

}  // namespace

auto main() -> int {
  bool ok = true;

  ok &= expect(!should_ask_for_password(false, Workaround::Input),
               "does not ask when auth token is disabled");
  ok &= expect(!should_ask_for_password(true, Workaround::Off),
               "does not ask when workaround is off");
  ok &= expect(should_ask_for_password(true, Workaround::Native),
               "asks when native workaround is enabled");

  {
    const auto plan = plan_prompt_stop(false, false, Workaround::Native);
    ok &= expect(!plan.stop_prompt, "inactive prompt is not stopped");
    ok &= expect(!plan.abort_prompt, "inactive prompt is not aborted");
    ok &= expect(!plan.send_enter, "inactive prompt does not send enter");
  }

  {
    const auto plan = plan_prompt_stop(true, true, Workaround::Input);
    ok &= expect(plan.stop_prompt, "ready prompt is joined");
    ok &= expect(!plan.abort_prompt, "ready prompt is not aborted");
    ok &= expect(!plan.send_enter, "ready prompt does not receive fake input");
  }

  {
    const auto plan = plan_prompt_stop(true, false, Workaround::Native);
    ok &= expect(plan.stop_prompt, "native workaround stops prompt");
    ok &= expect(plan.abort_prompt, "native workaround aborts prompt");
    ok &= expect(!plan.send_enter, "native workaround skips fake input");
  }

  {
    const auto plan = plan_prompt_stop(true, false, Workaround::Input);
    ok &= expect(plan.stop_prompt, "input workaround stops prompt");
    ok &= expect(!plan.abort_prompt, "input workaround does not abort");
    ok &= expect(plan.send_enter, "input workaround injects enter");
  }

  return ok ? 0 : 1;
}
