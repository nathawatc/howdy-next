#include "prompt_workaround.hpp"
#include "main.hpp"

#include <array>
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

  ok &= expect(!should_ask_for_password(false, Workaround::Input, false),
               "does not ask when auth token is disabled");
  ok &= expect(!should_ask_for_password(true, Workaround::Off, false),
               "does not ask when workaround is off");
  ok &= expect(!should_ask_for_password(true, Workaround::Native, true),
               "does not ask when an auth token already exists");
  ok &= expect(should_ask_for_password(true, Workaround::Native, false),
               "asks when native workaround is enabled");
  ok &= expect(get_workaround("off") == Workaround::Off,
               "parses off workaround");
  ok &= expect(get_workaround("input") == Workaround::Input,
               "parses input workaround");
  ok &= expect(get_workaround("native") == Workaround::Native,
               "parses native workaround");
  ok &= expect(get_workaround("unknown") == Workaround::Off,
               "unknown workaround falls back to off");
  {
    std::array<const char *, 1> args = {"workaround=native"};
    ok &= expect(get_pam_workaround(static_cast<int>(args.size()), args.data()) ==
                     Workaround::Native,
                 "parses native workaround from PAM args");
  }
  {
    std::array<const char *, 2> args = {"debug", "workaround=input"};
    ok &= expect(get_pam_workaround(static_cast<int>(args.size()), args.data()) ==
                     Workaround::Input,
                 "parses input workaround from PAM args");
  }
  {
    std::array<const char *, 1> args = {"workaround=invalid"};
    ok &= expect(get_pam_workaround(static_cast<int>(args.size()), args.data()) ==
                     Workaround::Off,
                 "invalid PAM workaround falls back to off");
  }
  ok &= expect(get_pam_workaround(0, nullptr) == Workaround::Off,
               "missing PAM workaround defaults to off");
  ok &= expect(get_pam_workaround(1, nullptr) == Workaround::Off,
               "null PAM args default to off");
  ok &= expect(!auth_token_item_present(nullptr),
               "null PAM auth token item is absent");
  ok &= expect(auth_token_item_present(""),
               "empty PAM auth token item is still present");
  ok &= expect(auth_token_item_present("password"),
               "non-empty PAM auth token item is present");

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
