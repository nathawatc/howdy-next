#include "support/rust_ffi.hpp"

// Linked like pam_howdy (--exclude-libs,ALL) so the exports test proves that no Rust symbol
// leaks from a module that embeds the howdy-ffi staticlib.
extern "C" __attribute__((visibility("default"))) auto HowdyRustFfiProbe() -> int;

extern "C" auto HowdyRustFfiProbe() -> int {
	return howdy::native::RustFfiAbiMatches() ? 0 : 1;
}
