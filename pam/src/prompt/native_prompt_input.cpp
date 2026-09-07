#include "prompt/native_prompt_input.hpp"

namespace howdy::pam::native_prompt_input {

	SensitiveBuffer::~SensitiveBuffer() {
		volatile char *cursor = data_.data();
		for (std::size_t index = 0; index < data_.size(); ++index) {
			cursor[index] = '\0';
		}
		length_ = 0;
	}

	auto SensitiveBuffer::Empty() const -> bool {
		return length_ == 0;
	}

	auto SensitiveBuffer::Full() const -> bool {
		return length_ == data_.size();
	}

	void SensitiveBuffer::PushBack(char value) {
		data_[length_++] = value;
	}

	void SensitiveBuffer::PopBack() {
		if (length_ > 0) {
			data_[--length_] = '\0';
		}
	}

	auto SensitiveBuffer::Data() const -> const char * {
		return data_.data();
	}

	auto SensitiveBuffer::Size() const -> std::size_t {
		return length_;
	}

	auto ProcessCharacter(char ch, SensitiveBuffer &password, bool &response_too_long)
	    -> CharacterResult {
		if (ch == '\n' || ch == '\r') {
			return CharacterResult::kComplete;
		}
		if (ch == 3) {
			return CharacterResult::kAbort;
		}
		if (ch == '\b' || ch == 127) {
			if (!password.Empty()) {
				password.PopBack();
			}
			return CharacterResult::kEepReading;
		}
		if (response_too_long) {
			return CharacterResult::kEepReading;
		}
		if (password.Full()) {
			response_too_long = true;
			return CharacterResult::kEepReading;
		}
		password.PushBack(ch);
		return CharacterResult::kEepReading;
	}

}  // namespace howdy::pam::native_prompt_input
