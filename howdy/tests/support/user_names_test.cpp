#include "support/user_names.hpp"
#include "test_support.hpp"

#include <string>
#include <string_view>

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
	// "<user>.dat" must fit in NAME_MAX (255 bytes).
	ok &= Expect(howdy::native::IsValidModelUserName(std::string(251, 'a')),
	             "251-byte username, whose model file name is 255 bytes, is accepted");
	ok &= Expect(!howdy::native::IsValidModelUserName(std::string(252, 'a')),
	             "252-byte username, whose model file name would exceed NAME_MAX, is rejected");
	ok &= Expect(!howdy::native::IsValidModelUserName("a\x01"
	                                                  "b"),
	             "username containing a control character is rejected");
	ok &= Expect(!howdy::native::IsValidModelUserName(std::string_view("a\0b", 3)),
	             "username containing NUL is rejected");
	ok &= Expect(!howdy::native::IsValidModelUserName("alice\nbob"),
	             "username containing newline is rejected");
	ok &= Expect(!howdy::native::IsValidModelUserName("."), "dot username is rejected");
	ok &= Expect(!howdy::native::IsValidModelUserName(".."), "dot-dot username is rejected");

	return ok ? 0 : 1;
}
