#pragma once

#include "config/config_limits.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <string>

#include <sys/stat.h>

namespace howdy::test {

	struct ConfigUtilsTestContext {
		std::filesystem::path temp_root;
		std::filesystem::path config_path;
		std::string           config_at_limit   = std::string(native::kMaxConfigFileSize, 'x');
		std::string           config_over_limit = std::string(native::kMaxConfigFileSize + 1, 'x');
	};

	inline auto WriteConfigTestFile(const std::filesystem::path &path, const std::string &content)
	    -> bool {
		const bool ok = WriteFile(path, content);
		if (ok && chmod(path.c_str(), 0644) != 0) {
			return false;
		}
		return ok;
	}

	auto RunConfigReadUpdateTests(ConfigUtilsTestContext &context) -> bool;
	auto RunConfigAtomicReplaceTests(ConfigUtilsTestContext &context) -> bool;
	auto RunConfigLockFailureTests(ConfigUtilsTestContext &context) -> bool;
	auto RunConfigAtomicReplaceTailTests(ConfigUtilsTestContext &context) -> bool;
	auto RunConfigPathSecurityTests(ConfigUtilsTestContext &context) -> bool;

}  // namespace howdy::test
