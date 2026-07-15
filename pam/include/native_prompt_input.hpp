#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace howdy::pam::native_prompt_input {

	constexpr std::size_t kMaxPromptResponseBytes = 512;

	class __attribute__((visibility("hidden"))) SensitiveBuffer {
	public:
		SensitiveBuffer()                                            = default;
		SensitiveBuffer(const SensitiveBuffer &)                     = delete;
		auto operator=(const SensitiveBuffer &) -> SensitiveBuffer & = delete;
		~SensitiveBuffer();

		[[nodiscard]] auto empty() const -> bool;
		[[nodiscard]] auto full() const -> bool;
		void               push_back(char value);
		void               pop_back();
		[[nodiscard]] auto data() const -> const char *;
		[[nodiscard]] auto size() const -> std::size_t;

	private:
		std::array<char, kMaxPromptResponseBytes> data_{};
		std::size_t                               length_ = 0;
	};

	enum class CharacterResult : std::uint8_t {
		keep_reading,
		complete,
		abort,
	};

	__attribute__((visibility("hidden"))) auto process_character(char ch, SensitiveBuffer &password,
	                                                             bool &response_too_long)
	    -> CharacterResult;

}  // namespace howdy::pam::native_prompt_input
