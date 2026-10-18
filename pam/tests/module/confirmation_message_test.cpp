#include "module/confirmation_message.hpp"
#include "test_support.hpp"

namespace {

	using howdy::test::Expect;

}  // namespace

auto main() -> int {
	bool ok = true;

	ok &= Expect(BuildConfirmationMessage("alice") == "Face matched user alice",
	             "confirmation message formats username into template");
	ok &= Expect(BuildConfirmationMessage("") == "Face matched user ",
	             "empty username formats template with empty value");

	return ok ? 0 : 1;
}
