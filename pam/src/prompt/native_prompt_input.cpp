#include "prompt/native_prompt_input.hpp"

#include <cstddef>

#include <string.h>
#include <termios.h>

namespace howdy::pam::native_prompt_input {

	namespace {
		constexpr cc_t kDelete = 0x7f;
	}  // namespace

	SensitiveBuffer::~SensitiveBuffer() {
		explicit_bzero(data_.data(), data_.size());
		length_ = 0;
	}

	auto SensitiveBuffer::Full() const -> bool {
		return length_ == data_.size();
	}

	void SensitiveBuffer::PushBack(char value) {
		data_.at(length_++) = value;
	}

	void SensitiveBuffer::PopCharacter(bool utf8) {
		while (length_ > 0) {
			const auto byte   = static_cast<unsigned char>(data_.at(--length_));
			data_.at(length_) = '\0';
			if (!utf8 || (byte & 0xC0U) != 0x80U) {
				return;
			}
		}
	}

	auto SensitiveBuffer::Data() const -> const char * {
		return data_.data();
	}

	auto SensitiveBuffer::Size() const -> std::size_t {
		return length_;
	}

	auto ProcessCharacter(char ch, const struct termios &terminal, SensitiveBuffer &password,
	                      bool &response_too_long) -> CharacterResult {
		// ICRNL turns Enter into '\n'; a '\r' still comes from typeahead stored before the prompt
		// switched modes (raw-mode callers).
		if (ch == '\n' || ch == '\r') {
			return CharacterResult::kComplete;
		}
		// NUL would truncate the C-string response; the others would have been signals.
		const auto byte = static_cast<cc_t>(ch);
		if (ch == '\0' || byte == terminal.c_cc[VINTR] || byte == terminal.c_cc[VQUIT] ||
		    byte == terminal.c_cc[VSUSP]) {
			return CharacterResult::kAbort;
		}
		// The kernel already applied the stty erase character; the other common Backspace byte
		// erases too, so a ^H/^? mismatch cannot hide in the password.
		if (ch == '\b' || byte == kDelete) {
			if (!response_too_long) {
				password.PopCharacter((terminal.c_iflag & IUTF8) != 0);
			}
			return CharacterResult::kKeepReading;
		}
		if (response_too_long) {
			return CharacterResult::kKeepReading;
		}
		if (password.Full()) {
			response_too_long = true;
			return CharacterResult::kKeepReading;
		}
		password.PushBack(ch);
		return CharacterResult::kKeepReading;
	}

}  // namespace howdy::pam::native_prompt_input
