#pragma once

#include "support/file_security/validation_root.hpp"

#include <sys/types.h>

namespace howdy::native {

	auto RunCompareEntrypoint(int argc, char **argv, uid_t trusted_owner_uid,
	                          const file_security_internal::ValidationRoot &validation_root = {})
	    -> int;

}  // namespace howdy::native
