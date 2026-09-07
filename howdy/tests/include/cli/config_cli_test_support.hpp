#pragma once

#include <ostream>
#include <streambuf>

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

	auto RunConfigCliCallbackExceptionTest() -> bool;
	auto RunConfigCliIntegrationTests() -> bool;
	auto RunConfigCliWorkflowTests() -> bool;

}  // namespace howdy::test::config_cli
