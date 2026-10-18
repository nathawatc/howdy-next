#include "storage/user_model_path.hpp"
#include "test_support.hpp"

#include <filesystem>

namespace {

	using howdy::test::Expect;

}  // namespace

auto main() -> int {
	bool ok = true;

	const std::filesystem::path models_dir = "/trusted/models";
	ok &=
	    Expect(howdy::native::ResolveUserModelPath(models_dir, "alice") == models_dir / "alice.dat",
		       "safe username resolves inside models directory");
	ok &= Expect(!howdy::native::ResolveUserModelPath(models_dir, "../alice").has_value(),
	             "unsafe username cannot escape models directory");
	ok &= Expect(!howdy::native::ResolveUserModelPath(models_dir, "alice..bob").has_value(),
	             "dot-dot username input is rejected before model path construction");

	return ok ? 0 : 1;
}
