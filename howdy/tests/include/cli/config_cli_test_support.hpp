#pragma once

#include "test_support.hpp"

namespace howdy::test::config_cli {

	using howdy::test::ScopedEnvironmentVariable;
	using howdy::test::ScopedStreamBuffer;

	auto RunConfigCliCallbackExceptionTest() -> bool;
	auto RunConfigCliIntegrationTests() -> bool;
	auto RunConfigCliWorkflowTests() -> bool;

}  // namespace howdy::test::config_cli
