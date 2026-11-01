#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <termios.h>

namespace howdy::pam::native_prompt_input {

	constexpr std::size_t kMaxPromptResponseBytes = 512;

	class __attribute__((visibility("hidden"))) SensitiveBuffer {
	public:
		SensitiveBuffer()                                            = default;
		SensitiveBuffer(const SensitiveBuffer &)                     = delete;
		auto operator=(const SensitiveBuffer &) -> SensitiveBuffer & = delete;
		SensitiveBuffer(SensitiveBuffer &&)                          = delete;
		auto operator=(SensitiveBuffer &&) -> SensitiveBuffer &      = delete;
		~SensitiveBuffer();

		[[nodiscard]] auto Full() const -> bool;
		void               PushBack(char value);
		// Removes one character: with `utf8`, its continuation bytes too.
		void               PopCharacter(bool utf8);
		[[nodiscard]] auto Data() const -> const char *;
		[[nodiscard]] auto Size() const -> std::size_t;

	private:
		std::array<char, kMaxPromptResponseBytes> data_{};
		std::size_t                               length_ = 0;
	};

	enum class CharacterResult : std::uint8_t {
		kKeepReading,
		kComplete,
		kAbort,
	};

	// Takes bytes of a line the kernel already edited (canonical mode); `terminal` supplies the
	// user's signal characters, which arrive as data because the prompt turns ISIG off.
	__attribute__((visibility("hidden"))) auto
	ProcessCharacter(char ch, const struct termios &terminal, SensitiveBuffer &password,
	                 bool &response_too_long) -> CharacterResult;

}  // namespace howdy::pam::native_prompt_input
