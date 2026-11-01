#pragma once

#include <cstddef>
#include <functional>
#include <ostream>
#include <sstream>
#include <streambuf>
#include <string>
#include <string_view>
#include <utility>

namespace howdy::native {

	// Redirects a stream into a private buffer until Release() or destruction, so the stream is
	// restored on every exit, including while an exception unwinds. When `on_line` is set, each
	// completed line is also handed to it as soon as the stream flushes, so a process killed
	// before Release() has already passed on every finished line.
	class ScopedStreamCapture {
	public:
		explicit ScopedStreamCapture(std::ostream                         &stream,
		                             std::function<void(std::string_view)> on_line = {})
		    : buffer_(std::move(on_line))
		    , stream_(&stream)
		    , previous_(stream.rdbuf(&buffer_)) {}

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
		class LineBuffer : public std::stringbuf {
		public:
			explicit LineBuffer(std::function<void(std::string_view)> on_line)
			    : on_line_(std::move(on_line)) {}

			// Hands over every remaining line, including a last one without a newline.
			void Finish() {
				(void)sync();
				const std::string_view text = view();
				if (on_line_ && emitted_ < text.size()) {
					on_line_(text.substr(emitted_));
					emitted_ = text.size();
				}
			}

		protected:
			auto sync() -> int override {
				if (!on_line_) {
					return 0;
				}
				const std::string_view text = view();
				for (auto end = text.find('\n', emitted_); end != std::string_view::npos;
				     end      = text.find('\n', emitted_)) {
					on_line_(text.substr(emitted_, end - emitted_));
					emitted_ = end + 1;
				}
				return 0;
			}

		private:
			std::function<void(std::string_view)> on_line_;
			std::size_t                           emitted_ = 0;
		};

		void Restore() {
			if (stream_ != nullptr) {
				stream_->rdbuf(previous_);
				buffer_.Finish();
				stream_ = nullptr;
			}
		}

		LineBuffer      buffer_;
		std::ostream   *stream_;
		std::streambuf *previous_;
	};

}  // namespace howdy::native
