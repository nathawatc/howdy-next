#pragma once

#include <INIReader.h>
#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace howdy::native {

	class ConfigReader {
	public:
		explicit ConfigReader(std::string path);
		ConfigReader(std::string path, std::string_view content);

		[[nodiscard]] auto Ok() const -> bool;
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
		std::string path_;
		INIReader   reader_;
	};

	inline auto ParseConfigFloatStrict(std::string_view value) -> std::optional<float> {
		if (value.empty()) {
			return std::nullopt;
		}

		const char *first = value.data();
		const char *last  = value.data() + value.size();
		if (*first == '+') {
			++first;
			if (first == last) {
				return std::nullopt;
			}
		}

		float      parsed = 0.0F;
		const auto result = std::from_chars(first, last, parsed);
		if (result.ec != std::errc{} || result.ptr != last || !std::isfinite(parsed)) {
			return std::nullopt;
		}

		return parsed;
	}

}  // namespace howdy::native
