#pragma once

#include "support/file_security/validation_root.hpp"

#include <sys/types.h>

namespace howdy::native {
	struct CompareEntrypointDependencies;

	auto RunCompareEntrypoint(int argc, char **argv, uid_t trusted_owner_uid,
	                          const file_security_internal::ValidationRoot &validation_root = {})
	    -> int;
	auto RunCompareEntrypoint(int argc, char **argv, uid_t trusted_owner_uid,
	                          const file_security_internal::ValidationRoot &validation_root,
	                          const CompareEntrypointDependencies          &dependencies) -> int;

}  // namespace howdy::native
