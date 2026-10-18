#include "storage/user_model_label.hpp"
#include "test_support.hpp"

namespace {

	using howdy::test::Expect;

}  // namespace

auto main() -> int {
	bool ok = true;

	ok &=
	    Expect(howdy::native::IsValidModelLabel("Laptop camera"), "normal model label is accepted");
	ok &= Expect(howdy::native::IsValidModelLabel(""), "empty model label remains accepted");
	ok &= Expect(!howdy::native::IsValidModelLabel("bad/name"),
	             "model label containing slash is rejected");
	ok &= Expect(!howdy::native::IsValidModelLabel("bad\\name"),
	             "model label containing backslash is rejected");
	ok &= Expect(!howdy::native::IsValidModelLabel("bad\nname"),
	             "model label containing newline is rejected");
	ok &= Expect(!howdy::native::IsValidModelLabel("bad\tname"),
	             "model label containing tab is rejected");

	return ok ? 0 : 1;
}
