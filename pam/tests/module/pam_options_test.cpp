#include "module/pam_options.hpp"
#include "test_support.hpp"

#include <array>
#include <string_view>

namespace {

	using howdy::pam::PamModuleArguments;
	using howdy::pam::parse_pam_options;
	using howdy::pam::Workaround;
	using howdy::test::expect;

	auto workaround_for(PamModuleArguments arguments) -> Workaround {
		return parse_pam_options(arguments).workaround;
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	ok &= expect(workaround_for({}) == Workaround::kOff, "no PAM arguments default to off");
	ok &= expect(workaround_for({.argc = 1, .argv = nullptr}) == Workaround::kOff,
	             "null PAM argv defaults to off");
	const std::array<const char *, 1> unrelated_args = {"debug"};
	ok &= expect(workaround_for({.argc = 0, .argv = unrelated_args.data()}) == Workaround::kOff,
	             "zero PAM argc defaults to off");
	ok &= expect(workaround_for({.argc = -1, .argv = unrelated_args.data()}) == Workaround::kOff,
	             "negative PAM argc defaults to off");

	const std::array<const char *, 3> null_entry_args = {nullptr, "debug", nullptr};
	ok &= expect(workaround_for({.argc = static_cast<int>(null_entry_args.size()),
	                             .argv = null_entry_args.data()}) == Workaround::kOff,
	             "null PAM argument entries are skipped");

	const std::array<const char *, 4> input_args = {"debug", "other=value", "workaround=input",
	                                                "after"};
	ok &= expect(workaround_for({.argc = static_cast<int>(input_args.size()),
	                             .argv = input_args.data()}) == Workaround::kInput,
	             "input workaround is parsed among unrelated arguments");

	const std::array<const char *, 1> native_args = {"workaround=native"};
	ok &= expect(workaround_for({.argc = 1, .argv = native_args.data()}) == Workaround::kNative,
	             "native workaround is parsed");

	const std::array<const char *, 1> native_input_args = {"workaround=native-input"};
	ok &= expect(workaround_for({.argc = 1, .argv = native_input_args.data()}) ==
	                 Workaround::kNativeInput,
	             "native-input workaround is parsed");

	for (const std::string_view argument :
	     {"workaround=", "workaround=unknown", "workaround=input=extra"}) {
		const char *const raw_argument = argument.data();
		ok &= expect(workaround_for({.argc = 1, .argv = &raw_argument}) == Workaround::kOff,
		             "empty, unknown, and malformed values default to off");
	}

	const std::array<const char *, 2> first_valid_duplicate = {"workaround=input",
	                                                           "workaround=native"};
	ok &= expect(workaround_for({.argc = 2, .argv = first_valid_duplicate.data()}) ==
	                 Workaround::kInput,
	             "duplicate workaround options use first option");

	const std::array<const char *, 2> first_native_duplicate = {"workaround=native",
	                                                            "workaround=input"};
	ok &= expect(workaround_for({.argc = 2, .argv = first_native_duplicate.data()}) ==
	                 Workaround::kNative,
	             "duplicate workaround order remains first-match");

	const std::array<const char *, 2> invalid_then_valid = {"workaround=invalid",
	                                                        "workaround=native"};
	ok &= expect(workaround_for({.argc = 2, .argv = invalid_then_valid.data()}) == Workaround::kOff,
	             "first invalid workaround prevents later duplicate from applying");

	const std::array<const char *, 1> config_style_args = {"core.workaround=input"};
	ok &= expect(workaround_for({.argc = 1, .argv = config_style_args.data()}) == Workaround::kOff,
	             "config-style workaround key is not a PAM option");

	return ok ? 0 : 1;
}
