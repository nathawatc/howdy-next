#pragma once

#include <ostream>
#include <sstream>
#include <streambuf>
#include <string>

namespace howdy::native {

	// Redirects a stream into a private buffer until Release() or destruction, so the stream is
	// restored on every exit, including while an exception unwinds.
	class ScopedStreamCapture {
	public:
		explicit ScopedStreamCapture(std::ostream &stream)
		    : stream_(&stream)
		    , previous_(stream.rdbuf(buffer_.rdbuf())) {}

		ScopedStreamCapture(const ScopedStreamCapture &)                     = delete;
		auto operator=(const ScopedStreamCapture &) -> ScopedStreamCapture & = delete;
		ScopedStreamCapture(ScopedStreamCapture &&)                          = delete;
		auto operator=(ScopedStreamCapture &&) -> ScopedStreamCapture &      = delete;

		~ScopedStreamCapture() {
			Restore();
		}

		// Restores the stream and returns everything written while it was captured.
		[[nodiscard]] auto Release() -> std::string {
			Restore();
			return buffer_.str();
		}

	private:
		void Restore() {
			if (stream_ != nullptr) {
				stream_->rdbuf(previous_);
				stream_ = nullptr;
			}
		}

		std::ostringstream buffer_;
		std::ostream      *stream_;
		std::streambuf    *previous_;
	};

}  // namespace howdy::native
