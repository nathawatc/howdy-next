#include "module/main.hpp"
#include "module/prompt_workaround.hpp"
#include "test_support.hpp"

#include <array>

namespace {

	using howdy::test::expect;

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

	return ok ? 0 : 1;
}
