#pragma once

#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <INIReader.h>

namespace howdy::native {

	// What a crash or an interrupted write leaves behind, which inih would read as a config that
	// sets nothing: defaults would then quietly replace settings such as `disabled`.
	enum class ConfigDamage : std::uint8_t {
		kNone,
		// Nothing but whitespace; a comment is enough to say that the defaults are meant.
		kBlank,
		// inih ends the line at the NUL and carries on.
		kNulByte,
	};

	[[nodiscard]] auto FindConfigDamage(std::string_view content) -> ConfigDamage;

	// Completes "Config file ...". Empty for kNone.
	[[nodiscard]] auto ConfigDamageDescription(ConfigDamage damage) -> std::string_view;

	class ConfigReader {
	public:
		explicit ConfigReader(std::string_view content);

		// Whether the content is undamaged and parsed.
		[[nodiscard]] auto Ok() const -> bool;
		[[nodiscard]] auto Damage() const -> ConfigDamage;
		[[nodiscard]] auto ParseError() const -> int;
		[[nodiscard]] auto Get(const std::string &section, const std::string &name,
		                       const std::string &fallback) const -> std::string;
		[[nodiscard]] auto GetInt(const std::string &section, const std::string &name,
		                          int fallback) const -> int;
		[[nodiscard]] auto GetFloat(const std::string &section, const std::string &name,
		                            float fallback) const -> float;
		[[nodiscard]] auto GetBool(const std::string &section, const std::string &name,
		                           bool fallback) const -> bool;
		[[nodiscard]] auto Sections() const -> std::vector<std::string>;
		[[nodiscard]] auto Keys(const std::string &section) const -> std::vector<std::string>;
		[[nodiscard]] auto HasValue(const std::string &section, const std::string &name) const
		    -> bool;

	private:
		ConfigDamage damage_ = ConfigDamage::kNone;
		INIReader    reader_;
	};

	[[nodiscard]] inline auto ParseConfigIntStrict(std::string_view value) -> std::optional<int> {
		if (value.empty()) {
			return std::nullopt;
		}

		if (value.front() == '+') {
			value.remove_prefix(1);
			if (value.empty() || value.front() == '-' || value.front() == '+') {
				return std::nullopt;
			}
		}

		int        parsed = 0;
		const auto result = std::from_chars(value.begin(), value.end(), parsed);
		if (result.ec != std::errc{} || result.ptr != value.end()) {
			return std::nullopt;
		}

		return parsed;
	}

	[[nodiscard]] inline auto ParseConfigFloatStrict(std::string_view value)
	    -> std::optional<float> {
		if (value.empty()) {
			return std::nullopt;
		}

		if (value.front() == '+') {
			value.remove_prefix(1);
			if (value.empty() || value.front() == '-' || value.front() == '+') {
				return std::nullopt;
			}
		}

		float      parsed = 0.0F;
		const auto result = std::from_chars(value.begin(), value.end(), parsed);
		if (result.ec != std::errc{} || result.ptr != value.end() || !std::isfinite(parsed)) {
			return std::nullopt;
		}

		return parsed;
	}

}  // namespace howdy::native
