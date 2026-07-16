#include "prompt/native_prompt_input.hpp"

namespace howdy::pam::native_prompt_input {

	SensitiveBuffer::~SensitiveBuffer() {
		volatile char *cursor = data_.data();
		for (std::size_t index = 0; index < data_.size(); ++index) {
			cursor[index] = '\0';
		}
		length_ = 0;
	}

	auto SensitiveBuffer::empty() const -> bool {
		return length_ == 0;
	}

	auto SensitiveBuffer::full() const -> bool {
		return length_ == data_.size();
	}

	void SensitiveBuffer::push_back(char value) {
		data_[length_++] = value;
	}

	void SensitiveBuffer::pop_back() {
		if (length_ > 0) {
			data_[--length_] = '\0';
		}
	}

	auto SensitiveBuffer::data() const -> const char * {
		return data_.data();
	}

	auto SensitiveBuffer::size() const -> std::size_t {
		return length_;
	}

	auto process_character(char ch, SensitiveBuffer &password, bool &response_too_long)
	    -> CharacterResult {
		if (ch == '\n' || ch == '\r') {
			return CharacterResult::complete;
		}
		if (ch == 3) {
			return CharacterResult::abort;
		}
		if (ch == '\b' || ch == 127) {
			if (!password.empty()) {
				password.pop_back();
			}
			return CharacterResult::keep_reading;
		}
		if (response_too_long) {
			return CharacterResult::keep_reading;
		}
		if (password.full()) {
			response_too_long = true;
			return CharacterResult::keep_reading;
		}
		password.push_back(ch);
		return CharacterResult::keep_reading;
	}

}  // namespace howdy::pam::native_prompt_input
