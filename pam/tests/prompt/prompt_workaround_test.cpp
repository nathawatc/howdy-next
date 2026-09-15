#include "prompt/workaround.hpp"
#include "test_support.hpp"

namespace {

	using howdy::pam::ShouldAskForPassword;
	using howdy::pam::Workaround;
	using howdy::test::Expect;

}  // namespace

auto main() -> int {
	bool ok = true;

	ok &= Expect(!ShouldAskForPassword(false, Workaround::kInput, false),
	             "does not ask when auth token is disabled");
	ok &= Expect(!ShouldAskForPassword(true, Workaround::kOff, false),
	             "does not ask when workaround is off");
	ok &= Expect(!ShouldAskForPassword(true, Workaround::kNative, true),
	             "does not ask when an auth token already exists");
	ok &= Expect(ShouldAskForPassword(true, Workaround::kNative, false),
	             "asks when native workaround is enabled");
	ok &= Expect(ShouldAskForPassword(true, Workaround::kInput, false),
	             "asks when input workaround is enabled");
	ok &= Expect(!ShouldAskForPassword(true, Workaround::kInput, true),
	             "input workaround does not ask when an auth token already exists");

	return ok ? 0 : 1;
}
