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

		[[nodiscard]] auto Empty() const -> bool;
		[[nodiscard]] auto Full() const -> bool;
		void               PushBack(char value);
		void               PopBack();
		[[nodiscard]] auto Data() const -> const char *;
		[[nodiscard]] auto Size() const -> std::size_t;

	private:
		std::array<char, kMaxPromptResponseBytes> data_{};
		std::size_t                               length_ = 0;
	};

	enum class CharacterResult : std::uint8_t {
		kEepReading,
		kComplete,
		kAbort,
	};

	__attribute__((visibility("hidden"))) auto ProcessCharacter(char ch, SensitiveBuffer &password,
	                                                            bool &response_too_long)
	    -> CharacterResult;

}  // namespace howdy::pam::native_prompt_input
