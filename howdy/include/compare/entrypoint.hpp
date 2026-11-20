#pragma once

#include <sys/types.h>

namespace howdy::native {
	struct CompareEntrypointDependencies;

	// Compare reads its config and model only from the descriptors PAM passes
	// (protocol/compare_descriptors.hpp), owned by `trusted_owner_uid` (root in production).
	auto RunCompareEntrypoint(int argc, char **argv, uid_t trusted_owner_uid) -> int;
	auto RunCompareEntrypoint(int argc, char **argv, uid_t trusted_owner_uid,
	                          const CompareEntrypointDependencies &dependencies) -> int;

}  // namespace howdy::native
