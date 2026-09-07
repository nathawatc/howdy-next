#include "module/pam_option_catalog.hpp"
#include "module/pam_options.hpp"
#include "test_support.hpp"

#include <array>
#include <string>
#include <string_view>

namespace {

	using howdy::pam::PamModuleArguments;
	using howdy::pam::ParsePamOptions;
	using howdy::pam::Workaround;
	using howdy::pam::WorkaroundCatalog;
	using howdy::test::expect;

	auto WorkaroundFor(PamModuleArguments arguments) -> Workaround {
		return ParsePamOptions(arguments).workaround;
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	ok &= expect(WorkaroundFor({}) == Workaround::kOff, "no PAM arguments default to off");
	ok &= expect(WorkaroundFor({.argc = 1, .argv = nullptr}) == Workaround::kOff,
	             "null PAM argv defaults to off");
	const std::array<const char *, 1> unrelated_args = {"debug"};
	ok &= expect(WorkaroundFor({.argc = 0, .argv = unrelated_args.data()}) == Workaround::kOff,
	             "zero PAM argc defaults to off");
	ok &= expect(WorkaroundFor({.argc = -1, .argv = unrelated_args.data()}) == Workaround::kOff,
	             "negative PAM argc defaults to off");

	const std::array<const char *, 3> null_entry_args = {nullptr, "debug", nullptr};
	ok &= expect(WorkaroundFor({.argc = static_cast<int>(null_entry_args.size()),
	                            .argv = null_entry_args.data()}) == Workaround::kOff,
	             "null PAM argument entries are skipped");

	const std::array<const char *, 4> input_args = {"debug", "other=value", "workaround=input",
	                                                "after"};
	ok &= expect(WorkaroundFor({.argc = static_cast<int>(input_args.size()),
	                            .argv = input_args.data()}) == Workaround::kInput,
	             "input workaround is parsed among unrelated arguments");

	const std::array<const char *, 1> native_args = {"workaround=native"};
	ok &= expect(WorkaroundFor({.argc = 1, .argv = native_args.data()}) == Workaround::kNative,
	             "native workaround is parsed");

	const std::array<const char *, 1> native_input_args = {"workaround=native-input"};
	ok &= expect(WorkaroundFor({.argc = 1, .argv = native_input_args.data()}) ==
	                 Workaround::kNativeInput,
	             "native-input workaround is parsed");

	for (const std::string_view argument :
	     {"workaround=", "workaround=off", "workaround=unknown", "workaround=input=extra"}) {
		const char *const raw_argument = argument.data();
		ok &= expect(WorkaroundFor({.argc = 1, .argv = &raw_argument}) == Workaround::kOff,
		             "empty, unknown, and malformed values default to off");
	}

	const std::array<const char *, 2> first_valid_duplicate = {"workaround=input",
	                                                           "workaround=native"};
	ok &= expect(WorkaroundFor({.argc = 2, .argv = first_valid_duplicate.data()}) ==
	                 Workaround::kInput,
	             "duplicate workaround options use first option");

	const std::array<const char *, 2> first_native_duplicate = {"workaround=native",
	                                                            "workaround=input"};
	ok &= expect(WorkaroundFor({.argc = 2, .argv = first_native_duplicate.data()}) ==
	                 Workaround::kNative,
	             "duplicate workaround order remains first-match");

	const std::array<const char *, 2> invalid_then_valid = {"workaround=invalid",
	                                                        "workaround=native"};
	ok &= expect(WorkaroundFor({.argc = 2, .argv = invalid_then_valid.data()}) == Workaround::kOff,
	             "first invalid workaround prevents later duplicate from applying");

	const std::array<const char *, 1> config_style_args = {"core.workaround=input"};
	ok &= expect(WorkaroundFor({.argc = 1, .argv = config_style_args.data()}) == Workaround::kOff,
	             "config-style workaround key is not a PAM option");

	constexpr std::array expected_values{"input", "native", "native-input"};
	const auto           catalog = WorkaroundCatalog();
	ok &= expect(catalog.size() == expected_values.size(), "workaround catalog size is stable");
	for (std::size_t index = 0; index < catalog.size(); ++index) {
		const auto &descriptor = catalog[index];
		ok &= expect(index < expected_values.size() && descriptor.value == expected_values[index],
		             "workaround catalog order is stable");
		ok &= expect(!descriptor.summary.empty(), "workaround summary is non-empty");
		const std::string argument     = "workaround=" + std::string(descriptor.value);
		const char *const raw_argument = argument.c_str();
		ok &= expect(WorkaroundFor({.argc = 1, .argv = &raw_argument}) == descriptor.workaround,
		             "parser accepts every catalog workaround value");
		for (std::size_t previous = 0; previous < index; ++previous) {
			ok &=
			    expect(catalog[previous].value != descriptor.value, "workaround values are unique");
		}
	}
	ok &=
	    expect(WorkaroundFor({}) == howdy::pam::kDefaultWorkaround, "catalog default remains off");

	return ok ? 0 : 1;
}
