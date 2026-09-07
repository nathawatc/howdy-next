#include "support/user_names.hpp"
#include "test_support.hpp"

#include <filesystem>

namespace {

	using howdy::test::expect;

}  // namespace

auto main() -> int {
	bool ok = true;

	ok &= expect(howdy::native::IsValidModelUserName("alice"), "normal username is accepted");
	ok &= expect(howdy::native::IsValidModelUserName("alice@example.com"),
	             "domain-style username is accepted");
	ok &= expect(!howdy::native::IsValidModelUserName(""), "empty username is rejected");
	ok &= expect(!howdy::native::IsValidModelUserName("alice/bob"),
	             "username containing slash is rejected");
	ok &= expect(!howdy::native::IsValidModelUserName("alice\\bob"),
	             "username containing backslash is rejected");
	ok &= expect(!howdy::native::IsValidModelUserName("../alice"),
	             "malformed path-traversal username input is rejected");
	ok &= expect(!howdy::native::IsValidModelUserName("alice..bob"),
	             "malformed username containing dot-dot is rejected");
	ok &= expect(!howdy::native::IsValidModelUserName(".alice"),
	             "hidden-file style username is rejected");
	ok &= expect(!howdy::native::IsValidModelUserName("alice bob"),
	             "username containing whitespace is rejected");

	const std::filesystem::path models_dir = "/trusted/models";
	ok &=
	    expect(howdy::native::ResolveUserModelPath(models_dir, "alice") == models_dir / "alice.dat",
	           "safe username resolves inside models directory");
	ok &= expect(!howdy::native::ResolveUserModelPath(models_dir, "../alice").has_value(),
	             "unsafe username cannot escape models directory");
	ok &= expect(!howdy::native::ResolveUserModelPath(models_dir, "alice..bob").has_value(),
	             "dot-dot username input is rejected before model path construction");

	ok &=
	    expect(howdy::native::IsValidModelLabel("Laptop camera"), "normal model label is accepted");
	ok &= expect(howdy::native::IsValidModelLabel(""), "empty model label remains accepted");
	ok &= expect(!howdy::native::IsValidModelLabel("bad/name"),
	             "model label containing slash is rejected");
	ok &= expect(!howdy::native::IsValidModelLabel("bad\\name"),
	             "model label containing backslash is rejected");
	ok &= expect(!howdy::native::IsValidModelLabel("bad\nname"),
	             "model label containing newline is rejected");
	ok &= expect(!howdy::native::IsValidModelLabel("bad\tname"),
	             "model label containing tab is rejected");

	return ok ? 0 : 1;
}
