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
		std::string config_at_limit   = "#" + std::string(native::kMaxConfigFileSize - 1, 'x');
		std::string config_over_limit = "#" + std::string(native::kMaxConfigFileSize, 'x');
	};

	inline auto WriteConfigTestFile(const std::filesystem::path &path, const std::string &content)
	    -> bool {
		const bool ok = WriteFile(path, content);
		if (ok && chmod(path.c_str(), 0644) != 0) {
			return false;
		}
		return ok;
	}

	inline auto ReadConfigTestFile(const std::filesystem::path &path) -> std::string {
		return ReadFile(path);
	}

	inline auto FailParentSync(const std::filesystem::path & /*path*/) -> bool {
		return false;
	}

	class FileSizeLimitGuard {
	public:
		FileSizeLimitGuard(const FileSizeLimitGuard &)                     = delete;
		auto operator=(const FileSizeLimitGuard &) -> FileSizeLimitGuard & = delete;
		FileSizeLimitGuard(FileSizeLimitGuard &&)                          = delete;
		auto operator=(FileSizeLimitGuard &&) -> FileSizeLimitGuard &      = delete;
		using SignalHandler                                                = void (*)(int);

		FileSizeLimitGuard()
		    : have_original_(getrlimit(RLIMIT_FSIZE, &original_) == 0) {
			if (have_original_) {
				previous_sigxfsz_ = std::signal(SIGXFSZ, SIG_IGN);
				signal_changed_   = previous_sigxfsz_ != SIG_ERR;
			}
		}

		[[nodiscard]] auto Available() const -> bool {
			return have_original_;
		}

		[[nodiscard]] auto Ready() const -> bool {
			return Available() && signal_changed_;
		}

		auto SetZero() -> bool {
			if (!Ready()) {
				return false;
			}

			auto zero_limit     = original_;
			zero_limit.rlim_cur = 0;
			if (setrlimit(RLIMIT_FSIZE, &zero_limit) != 0) {
				return false;
			}

			limit_changed_ = true;
			return true;
		}

		auto Restore() -> bool {
			bool ok = true;
			if (limit_changed_) {
				ok             = setrlimit(RLIMIT_FSIZE, &original_) == 0;
				limit_changed_ = false;
			}
			if (signal_changed_) {
				ok              = std::signal(SIGXFSZ, previous_sigxfsz_) != SIG_ERR && ok;
				signal_changed_ = false;
			}
			return ok;
		}

		~FileSizeLimitGuard() {
			(void)Restore();
		}

	private:
		rlimit        original_{};
		bool          have_original_    = false;
		bool          limit_changed_    = false;
		SignalHandler previous_sigxfsz_ = SIG_DFL;
		bool          signal_changed_   = false;
	};

	auto RunConfigReadUpdateTests(ConfigUtilsTestContext &context) -> bool;
	auto RunConfigAtomicReplaceTests(ConfigUtilsTestContext &context) -> bool;
	auto RunConfigLockFailureTests(ConfigUtilsTestContext &context) -> bool;
	auto RunConfigAtomicReplaceTailTests(ConfigUtilsTestContext &context) -> bool;
	auto RunConfigPathSecurityTests(ConfigUtilsTestContext &context) -> bool;

}  // namespace howdy::test
