#include "main.hpp"
#include "prompt_workaround.hpp"

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
	ok &= expect(should_ask_for_password(true, Workaround::Input, false),
	             "asks when input workaround is enabled");
	ok &= expect(!should_ask_for_password(true, Workaround::Input, true),
	             "input workaround does not ask when an auth token already exists");
	ok &= expect(get_workaround("off") == Workaround::Off, "parses off workaround");
	ok &= expect(get_workaround("input") == Workaround::Input, "parses input workaround");
	ok &= expect(get_workaround("native") == Workaround::Native, "parses native workaround");
	ok &= expect(get_workaround("native-input") == Workaround::NativeInput,
	             "parses native-input workaround");
	ok &= expect(get_workaround("unknown") == Workaround::Off,
	             "unknown workaround falls back to off");
	ok &= expect(get_workaround("input=extra") == Workaround::Off,
	             "malformed workaround value falls back to off");
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
		std::array<const char *, 2> args = {nullptr, "workaround=native"};
		ok &= expect(get_pam_workaround(static_cast<int>(args.size()), args.data()) ==
		                 Workaround::Native,
		             "skips null PAM args");
	}
	{
		std::array<const char *, 1> args = {"workaround=native-input"};
		ok &= expect(get_pam_workaround(static_cast<int>(args.size()), args.data()) ==
		                 Workaround::NativeInput,
		             "parses native-input workaround from PAM args");
	}
	{
		std::array<const char *, 1> args = {"workaround="};
		ok &= expect(get_pam_workaround(static_cast<int>(args.size()), args.data()) ==
		                 Workaround::Off,
		             "empty PAM workaround falls back to off");
	}
	{
		std::array<const char *, 1> args = {"workaround=invalid"};
		ok &= expect(get_pam_workaround(static_cast<int>(args.size()), args.data()) ==
		                 Workaround::Off,
		             "invalid PAM workaround falls back to off");
	}
	{
		std::array<const char *, 2> args = {"workaround=input", "workaround=native"};
		ok &= expect(get_pam_workaround(static_cast<int>(args.size()), args.data()) ==
		                 Workaround::Input,
		             "duplicate PAM workaround uses first option");
	}
	{
		std::array<const char *, 2> args = {"workaround=native", "workaround=input"};
		ok &= expect(get_pam_workaround(static_cast<int>(args.size()), args.data()) ==
		                 Workaround::Native,
		             "duplicate PAM workaround order remains explicit");
	}
	{
		std::array<const char *, 1> args = {"core.workaround=input"};
		ok &= expect(get_pam_workaround(static_cast<int>(args.size()), args.data()) ==
		                 Workaround::Off,
		             "config-style workaround key is not a PAM option");
	}
	ok &= expect(get_pam_workaround(0, nullptr) == Workaround::Off,
	             "missing PAM workaround defaults to off");
	ok &= expect(get_pam_workaround(1, nullptr) == Workaround::Off, "null PAM args default to off");
	ok &= expect(!auth_token_item_present(nullptr), "null PAM auth token item is absent");
	ok &= expect(auth_token_item_present(""), "empty PAM auth token item is still present");
	ok &= expect(auth_token_item_present("password"), "non-empty PAM auth token item is present");

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
		const auto plan = plan_prompt_stop(true, true, Workaround::Native);
		ok &= expect(plan.stop_prompt, "ready native prompt is joined");
		ok &= expect(!plan.abort_prompt, "ready native prompt is not aborted");
		ok &= expect(!plan.send_enter, "ready native prompt does not receive fake input");
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

	{
		const auto plan = plan_prompt_stop(true, false, Workaround::Off);
		ok &= expect(plan.stop_prompt, "off workaround stops pending prompt");
		ok &= expect(!plan.abort_prompt, "off workaround does not abort prompt");
		ok &= expect(!plan.send_enter, "off workaround does not inject enter");
	}

	return ok ? 0 : 1;
}
