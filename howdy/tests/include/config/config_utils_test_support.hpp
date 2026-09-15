#pragma once

#include "config/config_limits.hpp"
#include "test_support.hpp"

#include <csignal>
#include <filesystem>
#include <string>

#include <sys/resource.h>
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
		if (ok) {
			if (chmod(path.c_str(), 0644) != 0) {
				return false;
			}
		}
		return ok;
	}

	inline auto ReadConfigTestFile(const std::filesystem::path &path) -> std::string {
		return ReadFile(path);
	}

	inline auto FailParentSync(const std::filesystem::path & /*path*/) -> bool {
		return false;
	}

	struct FileSizeLimitGuard {
		using SignalHandler = void (*)(int);

		rlimit        original{};
		bool          have_original    = false;
		bool          limit_changed    = false;
		SignalHandler previous_sigxfsz = SIG_DFL;
		bool          signal_changed   = false;

		FileSizeLimitGuard()
		    : have_original(getrlimit(RLIMIT_FSIZE, &original) == 0) {
			if (have_original) {
				previous_sigxfsz = std::signal(SIGXFSZ, SIG_IGN);
				signal_changed   = previous_sigxfsz != SIG_ERR;
			}
		}

		[[nodiscard]] auto Ready() const -> bool {
			return have_original && signal_changed;
		}

		auto SetZero() -> bool {
			if (!Ready()) {
				return false;
			}

			auto zero_limit     = original;
			zero_limit.rlim_cur = 0;
			if (setrlimit(RLIMIT_FSIZE, &zero_limit) != 0) {
				return false;
			}

			limit_changed = true;
			return true;
		}

		auto Restore() -> bool {
			bool ok = true;
			if (limit_changed) {
				ok            = setrlimit(RLIMIT_FSIZE, &original) == 0;
				limit_changed = false;
			}
			if (signal_changed) {
				ok             = std::signal(SIGXFSZ, previous_sigxfsz) != SIG_ERR && ok;
				signal_changed = false;
			}
			return ok;
		}

		~FileSizeLimitGuard() {
			(void)Restore();
		}
	};

	auto RunConfigReadUpdateTests(ConfigUtilsTestContext &context) -> bool;
	auto RunConfigAtomicWriteTests(ConfigUtilsTestContext &context) -> bool;
	auto RunConfigAtomicReplaceTests(ConfigUtilsTestContext &context) -> bool;
	auto RunConfigLockFailureTests(ConfigUtilsTestContext &context) -> bool;
	auto RunConfigAtomicReplaceTailTests(ConfigUtilsTestContext &context) -> bool;
	auto RunConfigPathSecurityTests(ConfigUtilsTestContext &context) -> bool;

}  // namespace howdy::test
