#include "support/rust_ffi.hpp"
#include "test_support.hpp"

auto main() -> int {
	using howdy::test::Expect;

	const bool ok = Expect(howdy::native::RustFfiAbiMatches(),
	                       "linked Rust library matches the generated FFI header");
	return ok ? 0 : 1;
}
