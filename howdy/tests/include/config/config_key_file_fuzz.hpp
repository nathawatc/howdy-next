#pragma once

#include "../../../src/config/config_utils/internal.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace howdy::test {

	struct KeyFileFuzzTarget {
		std::string_view section;
		std::string_view key;
		std::string_view value;
	};

	// Replaces the target's value in `content` and aborts unless inih reads the outcome as the
	// update promises: the target as exactly the new value, and everything else as before.
	auto CheckKeyFileReplacement(const KeyFileFuzzTarget &target, std::string_view content)
	    -> native::config_utils_internal::ConfigLineReplaceResult;

	// Decodes a fuzz input and checks it. nullopt when it holds no case to check.
	auto RunKeyFileFuzzInput(std::span<const std::uint8_t> input)
	    -> std::optional<native::config_utils_internal::ConfigLineReplaceResult>;

}  // namespace howdy::test
