#pragma once

namespace howdy::native {

	// True when the linked Rust library reports the ABI version of the generated header.
	auto RustFfiAbiMatches() -> bool;

}  // namespace howdy::native
