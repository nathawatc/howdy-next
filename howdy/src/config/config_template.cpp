#include "config/config_template.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace howdy::native::config_template {
	namespace {

		auto OptionName(const config_schema::Option &option) -> std::string {
			return std::string(option.section) + "." + std::string(option.key);
		}

		auto Failure(std::string message) -> ConfigTemplateRenderResult {
			return {.ok = false, .error = std::move(message)};
		}

		auto IsAsciiSpace(char value) -> bool {
			return value == ' ' || value == '\t' || value == '\n' || value == '\r' ||
			       value == '\f' || value == '\v';
		}

		auto IsSafeIniName(std::string_view value) -> bool {
			if (!value.empty() && (IsAsciiSpace(value.front()) || IsAsciiSpace(value.back()))) {
				return false;
			}
			return std::ranges::all_of(value, [](const char raw_character) -> bool {
				const auto character = static_cast<unsigned char>(raw_character);
				return character >= 0x20U && character != 0x7fU && character != '[' &&
				       character != ']' && character != ';' && character != '#' && character != '=';
			});
		}

		auto IsSafeDescription(std::string_view value) -> bool {
			return !value.empty() &&
			       std::ranges::all_of(value, [](const char raw_character) -> bool {
				       const auto character = static_cast<unsigned char>(raw_character);
				       return character >= 0x20U && character != 0x7fU;
			       });
		}

		auto IsSafeIniScalar(std::string_view value) -> bool {
			if (!value.empty() && (IsAsciiSpace(value.front()) || IsAsciiSpace(value.back()))) {
				return false;
			}
			return std::ranges::all_of(value, [](const char raw_character) -> bool {
				const auto character = static_cast<unsigned char>(raw_character);
				return character >= 0x20U && character != 0x7fU && character != ';' &&
				       character != '#' && character != '=' && character != '[' &&
				       character != ']' && character != '\'' && character != '"';
			});
		}

		auto FormatFallback(const config_schema::Option &option) -> std::optional<std::string> {
			if (option.type == config_schema::ValueType::kString &&
			    !IsSafeIniScalar(option.fallback.string)) {
				return std::nullopt;
			}
			return config_schema::FormatFallbackValue(option);
		}

		auto ValidateRenderingConstraints(const config_schema::Option &option)
		    -> std::optional<std::string> {
			const auto name = OptionName(option);
			if (!IsSafeIniName(option.section)) {
				return "invalid or empty section: " + name;
			}
			if (!IsSafeIniName(option.key)) {
				return "invalid or empty key: " + name;
			}
			if (!IsSafeDescription(option.description)) {
				return "empty or unsafe description: " + name;
			}
			if (option.description == option.key) {
				return "description repeats key: " + name;
			}
			return std::nullopt;
		}

		auto SectionReuseError(const std::vector<std::string_view> &seen_sections,
		                       std::string_view                     current_section,
		                       const config_schema::Option &option, bool starts_section)
		    -> std::optional<std::string> {
			if (!starts_section || option.section == current_section) {
				return std::nullopt;
			}
			if (std::ranges::find(seen_sections, option.section) != seen_sections.end()) {
				return "section reused non-contiguously: " + std::string(option.section);
			}
			return std::nullopt;
		}

	}  // namespace

	auto RenderDefaultConfig(std::span<const config_schema::Option> options)
	    -> ConfigTemplateRenderResult {
		if (const auto validation = config_schema::ValidateOptions(options)) {
			return Failure(*validation);
		}
		if (options.empty()) {
			return Failure("config schema has no options");
		}

		std::vector<std::string_view> seen_sections;
		seen_sections.reserve(options.size());

		std::string      rendered = "# See howdy.ini(5) for configuration options.\n\n";
		std::string_view current_section;
		for (std::size_t index = 0; index < options.size(); ++index) {
			const auto &option = options[index];
			const auto  name   = OptionName(option);

			if (const auto error = ValidateRenderingConstraints(option)) {
				return Failure(*error);
			}

			const bool starts_section = index == 0 || option.section != current_section;
			if (const auto error =
			        SectionReuseError(seen_sections, current_section, option, starts_section)) {
				return Failure(*error);
			}
			if (starts_section) {
				if (index != 0) {
					rendered += '\n';
				}
				seen_sections.push_back(option.section);
				current_section = option.section;
				rendered += '[';
				rendered += option.section;
				rendered += "]\n";
			}

			const auto value = FormatFallback(option);
			if (!value.has_value()) {
				return Failure("cannot serialize fallback as INI scalar: " + name);
			}

			rendered += option.key;
			rendered += " = ";
			rendered += *value;
			rendered += '\n';
		}

		return {.ok = true, .content = std::move(rendered)};
	}

}  // namespace howdy::native::config_template
