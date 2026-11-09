#include "config/config_template.hpp"

#include "config/config_schema.hpp"
#include "config_document/internal.hpp"

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace howdy::native::config_template {
	namespace {

		auto Failure(std::string message) -> ConfigTemplateRenderResult {
			return {.ok = false, .error = std::move(message)};
		}

		// Table and key names stay TOML bare keys, so the file reads naturally and matches the
		// names documented in howdy.toml(5).
		auto IsBareKey(std::string_view value) -> bool {
			return !value.empty() && std::ranges::all_of(value, [](const char character) -> bool {
				return (character >= 'a' && character <= 'z') ||
				       (character >= 'A' && character <= 'Z') ||
				       (character >= '0' && character <= '9') || character == '_' ||
				       character == '-';
			});
		}

		auto IsSafeDescription(std::string_view value) -> bool {
			return !value.empty() &&
			       std::ranges::all_of(value, [](const char raw_character) -> bool {
				       const auto character = static_cast<unsigned char>(raw_character);
				       return character >= 0x20U && character != 0x7fU;
			       });
		}

		auto ValidateRenderingConstraints(const config_schema::Option &option)
		    -> std::optional<std::string> {
			const auto name = OptionName(option);
			if (!IsBareKey(option.section)) {
				return "invalid or empty section: " + name;
			}
			if (!IsBareKey(option.key)) {
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

		std::string      rendered = "# See howdy.toml(5) for configuration options.\n\n";
		std::string_view current_section;
		bool             first_option = true;
		for (const auto &option : options) {
			const auto name = OptionName(option);

			if (const auto error = ValidateRenderingConstraints(option)) {
				return Failure(*error);
			}

			const bool starts_section = first_option || option.section != current_section;
			if (const auto error =
			        SectionReuseError(seen_sections, current_section, option, starts_section)) {
				return Failure(*error);
			}
			if (starts_section) {
				seen_sections.push_back(option.section);
				current_section = option.section;
			}

			const auto value = config_schema::FallbackConfigValue(option);
			if (!value.has_value()) {
				return Failure("option has no fallback value: " + name);
			}
			// The Rust TOML editor is the only TOML encoder: it appends each table and quotes or
			// escapes values, so the template and `howdy set` write identical syntax.
			auto edited = SetConfigValue(rendered, option, *value);
			if (!edited.ok) {
				return Failure("cannot serialize fallback as TOML: " + name + ": " + edited.error);
			}
			rendered     = std::move(edited.content);
			first_option = false;
		}

		return {.ok = true, .content = std::move(rendered)};
	}

}  // namespace howdy::native::config_template
