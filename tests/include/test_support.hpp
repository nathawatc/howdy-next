#pragma once

#include "support/scoped_fd.hpp"

#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <stdlib.h>

#include <sys/resource.h>
#include <sys/stat.h>

namespace howdy::test {

	// Fixtures are created with plain streams and mkdir; the secure-path checks under test reject
	// group-writable files, so pin the umask instead of inheriting the caller's shell setting.
	inline const mode_t kPinnedTestUmask = umask(022);

	inline auto Expect(bool condition, std::string_view message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << '\n';
		}
		return condition;
	}

	inline void Require(bool condition, std::string_view message) {
		if (!condition) {
			throw std::runtime_error(std::string(message));
		}
	}

	inline auto FailParentSync(const std::filesystem::path & /*path*/) -> bool {
		return false;
	}

	using ScopedFd = howdy::native::ScopedFd;

	struct NonStandardExceptionForTest {};

	[[noreturn]] inline void ThrowNonStandardExceptionForTest() {
		std::rethrow_exception(std::make_exception_ptr(NonStandardExceptionForTest{}));
	}

	template <typename Actual, typename Expected, typename Tolerance>
	inline auto ExpectNear(Actual actual, Expected expected, Tolerance tolerance,
	                       std::string_view message) -> bool {
		return Expect(std::fabs(actual - expected) <= tolerance, message);
	}

	inline auto ReadFile(const std::filesystem::path &path) -> std::string {
		std::ifstream input(path, std::ios::binary);
		if (!input.is_open()) {
			return {};
		}
		return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	}

	inline auto WriteFile(const std::filesystem::path &path, std::string_view content) -> bool {
		std::ofstream output(path, std::ios::binary);
		if (!output.is_open()) {
			return false;
		}
		output.write(content.data(), static_cast<std::streamsize>(content.size()));
		return output.good();
	}

	inline auto CountFilesWithPrefix(const std::filesystem::path &directory,
	                                 std::string_view             prefix) -> std::size_t {
		std::size_t count = 0;
		for (const auto &entry : std::filesystem::directory_iterator(directory)) {
			if (entry.path().filename().string().starts_with(prefix)) {
				++count;
			}
		}
		return count;
	}

	inline auto NestedArray(std::size_t depth) -> std::string {
		return std::string(depth, '[') + "0" + std::string(depth, ']');
	}

	class StreamRedirect {
	public:
		StreamRedirect(const StreamRedirect &)                     = delete;
		auto operator=(const StreamRedirect &) -> StreamRedirect & = delete;
		StreamRedirect(StreamRedirect &&)                          = delete;
		auto operator=(StreamRedirect &&) -> StreamRedirect &      = delete;

		StreamRedirect(std::ios &stream, std::streambuf *new_buffer)
		    : first_(&stream)
		    , old_first_(stream.rdbuf(new_buffer)) {}

		StreamRedirect(std::ios &first_stream, std::streambuf *new_first, std::ios &second_stream,
		               std::streambuf *new_second)
		    : first_(&first_stream)
		    , second_(&second_stream)
		    , old_first_(first_stream.rdbuf(new_first))
		    , old_second_(second_stream.rdbuf(new_second)) {}

		~StreamRedirect() noexcept {
			if (first_ != nullptr) {
				first_->rdbuf(old_first_);
			}
			if (second_ != nullptr) {
				second_->rdbuf(old_second_);
			}
		}

	private:
		std::ios       *first_      = nullptr;
		std::ios       *second_     = nullptr;
		std::streambuf *old_first_  = nullptr;
		std::streambuf *old_second_ = nullptr;
	};

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

	class ScopedEnvironmentVariable {
	public:
		ScopedEnvironmentVariable(const ScopedEnvironmentVariable &)                     = delete;
		auto operator=(const ScopedEnvironmentVariable &) -> ScopedEnvironmentVariable & = delete;
		ScopedEnvironmentVariable(ScopedEnvironmentVariable &&)                          = delete;
		auto operator=(ScopedEnvironmentVariable &&) -> ScopedEnvironmentVariable &      = delete;

		ScopedEnvironmentVariable(const char *name, std::string_view value)
		    : name_(name) {
			SaveOriginal();
			Set(value);
		}

		explicit ScopedEnvironmentVariable(const char *name)
		    : name_(name) {
			SaveOriginal();
			Unset();
		}

		~ScopedEnvironmentVariable() noexcept {
			if (original_.has_value()) {
				setenv(name_.c_str(), original_->c_str(), 1);
			} else {
				unsetenv(name_.c_str());
			}
		}

		void Set(std::string_view value) {
			const std::string storage(value);
			setenv(name_.c_str(), storage.c_str(), 1);
		}

		void Unset() {
			unsetenv(name_.c_str());
		}

	private:
		void SaveOriginal() {
			if (const char *current = std::getenv(name_.c_str()); current != nullptr) {
				original_ = current;
			}
		}

		std::string                name_;
		std::optional<std::string> original_;
	};

	class TemporaryDirectory {
	public:
		explicit TemporaryDirectory(std::filesystem::path directory_path)
		    : path_(std::move(directory_path)) {}

		TemporaryDirectory(const TemporaryDirectory &)                     = delete;
		auto operator=(const TemporaryDirectory &) -> TemporaryDirectory & = delete;
		TemporaryDirectory(TemporaryDirectory &&)                          = delete;
		auto operator=(TemporaryDirectory &&) -> TemporaryDirectory &      = delete;

		~TemporaryDirectory() noexcept {
			std::error_code ec;
			std::filesystem::remove_all(path_, ec);
		}

		[[nodiscard]] auto Path() const -> const std::filesystem::path & {
			return path_;
		}

	private:
		std::filesystem::path path_;
	};

	inline auto CreateTempDirectory(std::string_view prefix)
	    -> std::optional<std::filesystem::path> {
		std::error_code ec;
		const auto      temp_root = std::filesystem::temp_directory_path(ec);
		if (ec) {
			return std::nullopt;
		}
		std::string       template_path = (temp_root / (std::string(prefix) + "XXXXXX")).string();
		const char *const created       = mkdtemp(template_path.data());
		if (created == nullptr) {
			return std::nullopt;
		}
		return std::filesystem::path(created);
	}

}  // namespace howdy::test
