#pragma once

#include <concepts>
#include <cstdint>
#include <type_traits>

namespace howdy::native {

	template <typename Value>
	concept BitValue = (std::integral<Value> && !std::same_as<std::remove_cv_t<Value>, bool>) ||
	                   std::is_enum_v<Value>;

	template <BitValue Value>
	[[nodiscard]] constexpr auto BitRepresentation(Value value) -> std::uintmax_t {
		if constexpr (std::is_enum_v<Value>) {
			using Underlying = std::underlying_type_t<Value>;
			using Unsigned   = std::make_unsigned_t<Underlying>;
			return static_cast<std::uintmax_t>(static_cast<Unsigned>(value));
		} else {
			using Unsigned = std::make_unsigned_t<Value>;
			return static_cast<std::uintmax_t>(static_cast<Unsigned>(value));
		}
	}

	template <BitValue... Values>
	[[nodiscard]] constexpr auto CombineBits(Values... values) -> std::uintmax_t {
		return (std::uintmax_t{0} | ... | BitRepresentation(values));
	}

	template <std::integral Result, BitValue... Values>
	[[nodiscard]] constexpr auto CombineBitsAs(Values... values) -> Result {
		return static_cast<Result>(CombineBits(values...));
	}

	template <BitValue Value, BitValue... Masks>
	[[nodiscard]] constexpr auto MaskedBits(Value value, Masks... masks) -> std::uintmax_t {
		return BitRepresentation(value) & CombineBits(masks...);
	}

	template <std::integral Result, BitValue Value, BitValue... Masks>
	[[nodiscard]] constexpr auto MaskedBitsAs(Value value, Masks... masks) -> Result {
		return static_cast<Result>(MaskedBits(value, masks...));
	}

	template <BitValue Value, BitValue... Masks>
	[[nodiscard]] constexpr auto HasAnyBits(Value value, Masks... masks) -> bool {
		return MaskedBits(value, masks...) != 0;
	}

	template <BitValue Value, BitValue... Masks>
	[[nodiscard]] constexpr auto HasAllBits(Value value, Masks... masks) -> bool {
		const auto expected = CombineBits(masks...);
		return MaskedBits(value, masks...) == expected;
	}

}  // namespace howdy::native
