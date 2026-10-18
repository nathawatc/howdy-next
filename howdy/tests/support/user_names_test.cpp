#include "support/user_names.hpp"
#include "test_support.hpp"

namespace {

	using howdy::test::Expect;

}  // namespace

auto main() -> int {
	bool ok = true;

	ok &= Expect(howdy::native::IsValidModelUserName("alice"), "normal username is accepted");
	ok &= Expect(howdy::native::IsValidModelUserName("alice@example.com"),
	             "domain-style username is accepted");
	ok &= Expect(!howdy::native::IsValidModelUserName(""), "empty username is rejected");
	ok &= Expect(!howdy::native::IsValidModelUserName("alice/bob"),
	             "username containing slash is rejected");
	ok &= Expect(!howdy::native::IsValidModelUserName("alice\\bob"),
	             "username containing backslash is rejected");
	ok &= Expect(!howdy::native::IsValidModelUserName("../alice"),
	             "malformed path-traversal username input is rejected");
	ok &= Expect(!howdy::native::IsValidModelUserName("alice..bob"),
	             "malformed username containing dot-dot is rejected");
	ok &= Expect(!howdy::native::IsValidModelUserName(".alice"),
	             "hidden-file style username is rejected");
	ok &= Expect(!howdy::native::IsValidModelUserName("-h"),
	             "short option-shaped username is rejected");
	ok &= Expect(!howdy::native::IsValidModelUserName("--help"),
	             "long option-shaped username is rejected");
	ok &= Expect(!howdy::native::IsValidModelUserName("alice bob"),
	             "username containing whitespace is rejected");

	return ok ? 0 : 1;
}
