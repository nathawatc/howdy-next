#include "config/config_template.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace howdy::native::config_template {
	namespace {

		auto option_name(const config_schema::Option &option) -> std::string {
			return std::string(option.section) + "." + std::string(option.key);
		}

		auto failure(std::string message) -> ConfigTemplateRenderResult {
			return {.ok = false, .error = std::move(message)};
		}

		auto is_ascii_space(char value) -> bool {
			return value == ' ' || value == '\t' || value == '\n' || value == '\r' ||
			       value == '\f' || value == '\v';
		}

		auto is_safe_ini_name(std::string_view value) -> bool {
			if (!value.empty() && (is_ascii_space(value.front()) || is_ascii_space(value.back()))) {
				return false;
			}
			return std::ranges::all_of(value, [](const char raw_character) -> bool {
				const auto character = static_cast<unsigned char>(raw_character);
				return character >= 0x20U && character != 0x7fU && character != '[' &&
				       character != ']' && character != ';' && character != '#' && character != '=';
			});
		}

		auto is_safe_description(std::string_view value) -> bool {
			return !value.empty() &&
			       std::ranges::all_of(value, [](const char raw_character) -> bool {
				       const auto character = static_cast<unsigned char>(raw_character);
				       return character >= 0x20U && character != 0x7fU;
			       });
		}

		auto is_safe_ini_scalar(std::string_view value) -> bool {
			if (!value.empty() && (is_ascii_space(value.front()) || is_ascii_space(value.back()))) {
				return false;
			}
			return std::ranges::all_of(value, [](const char raw_character) -> bool {
				const auto character = static_cast<unsigned char>(raw_character);
				return character >= 0x20U && character != 0x7fU && character != ';' &&
				       character != '#' && character != '=' && character != '[' &&
				       character != ']' && character != '\'' && character != '"';
			});
		}

		auto format_integer(int value) -> std::optional<std::string> {
			std::array<char, 32> buffer{};
			const auto           result =
			    std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, 10);
			if (result.ec != std::errc{}) {
				return std::nullopt;
			}
			return std::string(buffer.data(), result.ptr);
		}

		auto format_float(float value) -> std::optional<std::string> {
			if (!std::isfinite(value)) {
				return std::nullopt;
			}
			std::array<char, 64> buffer{};
			const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
			                                  std::chars_format::general);
			if (result.ec != std::errc{}) {
				return std::nullopt;
			}
			return std::string(buffer.data(), result.ptr);
		}

		auto format_fallback(const config_schema::Option &option) -> std::optional<std::string> {
			switch (option.type) {
				case config_schema::ValueType::boolean:
					return option.fallback.boolean ? "true" : "false";
				case config_schema::ValueType::integer:
					return format_integer(option.fallback.integer);
				case config_schema::ValueType::floating_point:
					return format_float(option.fallback.floating_point);
				case config_schema::ValueType::string:
					if (!is_safe_ini_scalar(option.fallback.string)) {
						return std::nullopt;
					}
					return std::string(option.fallback.string);
			}
			return std::nullopt;
		}

		auto validate_rendering_constraints(const config_schema::Option &option)
		    -> std::optional<std::string> {
			const auto name = option_name(option);
			if (!is_safe_ini_name(option.section)) {
				return "invalid or empty section: " + name;
			}
			if (!is_safe_ini_name(option.key)) {
				return "invalid or empty key: " + name;
			}
			if (!is_safe_description(option.description)) {
				return "empty or unsafe description: " + name;
			}
			if (option.description == option.key) {
				return "description repeats key: " + name;
			}
			return std::nullopt;
		}

		auto section_reuse_error(const std::vector<std::string_view> &seen_sections,
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

	auto render_default_config(std::span<const config_schema::Option> options)
	    -> ConfigTemplateRenderResult {
		if (const auto validation = config_schema::validate_options(options)) {
			return failure(*validation);
		}
		if (options.empty()) {
			return failure("config schema has no options");
		}

		std::vector<std::string_view> seen_sections;
		seen_sections.reserve(options.size());

		std::string      rendered;
		std::string_view current_section;
		for (std::size_t index = 0; index < options.size(); ++index) {
			const auto &option = options[index];
			const auto  name   = option_name(option);

			if (const auto error = validate_rendering_constraints(option)) {
				return failure(*error);
			}

			const bool starts_section = index == 0 || option.section != current_section;
			if (const auto error =
			        section_reuse_error(seen_sections, current_section, option, starts_section)) {
				return failure(*error);
			}
			if (starts_section) {
				seen_sections.push_back(option.section);
				current_section = option.section;
			}

			const auto value = format_fallback(option);
			if (!value.has_value()) {
				return failure("cannot serialize fallback as INI scalar: " + name);
			}

			if (index != 0) {
				rendered += '\n';
			}
			if (index == 0 || option.section != options[index - 1].section) {
				rendered += '[';
				rendered += option.section;
				rendered += "]\n";
			}
			rendered += "# ";
			rendered += option.description;
			rendered += "\n";
			rendered += option.key;
			rendered += " = ";
			rendered += *value;
			if (index + 1 < options.size()) {
				rendered += '\n';
			}
		}
		rendered += '\n';

		return {.ok = true, .content = std::move(rendered)};
	}

}  // namespace howdy::native::config_template
