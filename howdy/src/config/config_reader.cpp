#include "config_reader/internal.hpp"
#include "support/ascii.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace howdy::native {

	auto FindConfigDamage(std::string_view content) -> ConfigDamage {
		if (content.contains('\0')) {
			return ConfigDamage::kNulByte;
		}
		constexpr std::string_view utf8_bom = "\xEF\xBB\xBF";
		if (content.starts_with(utf8_bom)) {
			content.remove_prefix(utf8_bom.size());
		}
		return std::ranges::all_of(content, IsAsciiSpace) ? ConfigDamage::kBlank
		                                                  : ConfigDamage::kNone;
	}

	auto ConfigDamageDescription(ConfigDamage damage) -> std::string_view {
		switch (damage) {
			case ConfigDamage::kBlank:
				return "is blank";
			case ConfigDamage::kNulByte:
				return "contains a NUL byte";
			case ConfigDamage::kNone:
				break;
		}
		return {};
	}

	ConfigReader::ConfigReader(std::string_view content)
	    : damage_(FindConfigDamage(content))
	    , reader_(content.data(), content.size()) {}

	auto ConfigReader::Ok() const -> bool {
		return damage_ == ConfigDamage::kNone && reader_.ParseError() == 0;
	}

	auto ConfigReader::Damage() const -> ConfigDamage {
		return damage_;
	}

	auto ConfigReader::ParseError() const -> int {
		return reader_.ParseError();
	}

	auto ConfigReader::Get(const std::string &section, const std::string &name,
	                       const std::string &fallback) const -> std::string {
		return reader_.Get(section, name, fallback);
	}

	auto ConfigReader::GetInt(const std::string &section, const std::string &name,
	                          int fallback) const -> int {
		const auto value = reader_.Get(section, name, "");
		if (const auto parsed = ParseConfigIntStrict(value)) {
			return *parsed;
		}
		return fallback;
	}

	auto ConfigReader::GetFloat(const std::string &section, const std::string &name,
	                            float fallback) const -> float {
		const auto value = reader_.Get(section, name, "");
		if (const auto parsed = ParseConfigFloatStrict(value)) {
			return *parsed;
		}
		return fallback;
	}

	auto ConfigReader::GetBool(const std::string &section, const std::string &name,
	                           bool fallback) const -> bool {
		return reader_.GetBoolean(section, name, fallback);
	}

	auto ConfigReader::Sections() const -> std::vector<std::string> {
		return reader_.Sections();
	}

	auto ConfigReader::Keys(const std::string &section) const -> std::vector<std::string> {
		return reader_.Keys(section);
	}

	auto ConfigReader::HasValue(const std::string &section, const std::string &name) const -> bool {
		return reader_.HasValue(section, name);
	}

}  // namespace howdy::native
