#pragma once

#include <cstdlib>
#include <optional>
#include <ostream>
#include <streambuf>
#include <string>

namespace howdy::test::config_cli {

	class ScopedStreamBuffer {
	public:
		ScopedStreamBuffer(std::ostream &stream, std::streambuf *replacement)
		    : stream_(stream)
		    , original_(stream.rdbuf(replacement)) {}

		ScopedStreamBuffer(const ScopedStreamBuffer &)                     = delete;
		auto operator=(const ScopedStreamBuffer &) -> ScopedStreamBuffer & = delete;

		~ScopedStreamBuffer() noexcept {
			stream_.rdbuf(original_);
		}

	private:
		std::ostream   &stream_;
		std::streambuf *original_;
	};

	class ScopedEnvironmentVariable {
	public:
		ScopedEnvironmentVariable(const char *name, const std::string &value)
		    : name_(name) {
			SaveOriginal();
			setenv(name_, value.c_str(), 1);
		}

		explicit ScopedEnvironmentVariable(const char *name)
		    : name_(name) {
			SaveOriginal();
			unsetenv(name_);
		}

		ScopedEnvironmentVariable(const ScopedEnvironmentVariable &)                     = delete;
		auto operator=(const ScopedEnvironmentVariable &) -> ScopedEnvironmentVariable & = delete;

		~ScopedEnvironmentVariable() noexcept {
			if (original_.has_value()) {
				setenv(name_, original_->c_str(), 1);
			} else {
				unsetenv(name_);
			}
		}

		void Set(const std::string &value) {
			setenv(name_, value.c_str(), 1);
		}

		void Unset() {
			unsetenv(name_);
		}

	private:
		void SaveOriginal() {
			if (const char *current = std::getenv(name_); current != nullptr) {
				original_ = current;
			}
		}

		const char                *name_;
		std::optional<std::string> original_;
	};

	auto RunConfigCliCallbackExceptionTest() -> bool;
	auto RunConfigCliIntegrationTests() -> bool;
	auto RunConfigCliWorkflowTests() -> bool;

}  // namespace howdy::test::config_cli
