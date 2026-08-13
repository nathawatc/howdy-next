#include "config/config_schema.hpp"
#include "config/config_template.hpp"
#include "test_support.hpp"

#include <array>
#include <cstddef>
#include <locale>
#include <span>
#include <string>
#include <string_view>

namespace {

	using howdy::native::config_schema::Option;
	using howdy::native::config_schema::OptionId;
	using howdy::native::config_schema::RuntimeDefault;
	using howdy::native::config_schema::ValueType;
	using howdy::test::expect;

	class comma_decimal_numpunct final : public std::numpunct<char> {
	protected:
		auto do_decimal_point() const -> char override {
			return ',';
		}
	};

	auto synthetic_option(OptionId id, std::string_view section, std::string_view key,
	                      ValueType type, RuntimeDefault fallback, std::string_view description)
	    -> Option {
		return Option{.id           = id,
		              .section      = section,
		              .key          = key,
		              .type         = type,
		              .fallback     = fallback,
		              .range        = {},
		              .choices      = {},
		              .special_rule = howdy::native::config_schema::SpecialRule::none,
		              .invalid_rule = "synthetic rule",
		              .description  = description};
	}

	auto render(const auto &options) -> howdy::native::config_template::ConfigTemplateRenderResult {
		return howdy::native::config_template::render_default_config(
		    std::span<const Option>(options));
	}

	auto rejects_rendering_error(const auto &options, std::string_view error_text) -> bool {
		const auto result = render(options);
		return expect(!result.ok, "rendering constraint is rejected") &&
		       expect(result.error.contains(error_text),
		              "renderer reports: " + std::string(error_text));
	}

	auto count_occurrences(std::string_view text, std::string_view needle) -> std::size_t {
		std::size_t count = 0;
		std::size_t start = 0;
		while (true) {
			const auto position = text.find(needle, start);
			if (position == std::string_view::npos) {
				break;
			}
			++count;
			start = position + needle.size();
		}
		return count;
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	const std::array primitive_options = {
	    synthetic_option(OptionId::core_detection_notice, "alpha", "enabled", ValueType::boolean,
	                     howdy::native::config_schema::bool_default(true),
	                     "Enable synthetic mode."),
	    synthetic_option(OptionId::core_no_confirmation, "alpha", "count", ValueType::integer,
	                     howdy::native::config_schema::int_default(320),
	                     "Number of samples to use."),
	    synthetic_option(OptionId::core_abort_if_ssh, "beta", "ratio", ValueType::floating_point,
	                     howdy::native::config_schema::float_default(1.25F),
	                     "Scaling ratio for synthetic mode."),
	    synthetic_option(OptionId::core_abort_if_lid_closed, "beta", "path", ValueType::string,
	                     howdy::native::config_schema::string_default("none"),
	                     "Device path used by synthetic mode."),
	};
	const auto primitive_result = render(primitive_options);
	ok &= expect(primitive_result.ok, "primitive schema renders");
	if (primitive_result.ok) {
		const auto previous_locale =
		    std::locale::global(std::locale(std::locale::classic(), new comma_decimal_numpunct));
		const auto locale_result = render(primitive_options);
		std::locale::global(previous_locale);
		ok &= expect(locale_result.ok && locale_result.content.contains("ratio = 1.25\n"),
		             "floating-point rendering ignores comma decimal locale");

		const std::string expected = "[alpha]\n"
		                             "# Enable synthetic mode.\n"
		                             "enabled = true\n\n"
		                             "# Number of samples to use.\n"
		                             "count = 320\n\n"
		                             "[beta]\n"
		                             "# Scaling ratio for synthetic mode.\n"
		                             "ratio = 1.25\n\n"
		                             "# Device path used by synthetic mode.\n"
		                             "path = none\n";
		ok &=
		    expect(primitive_result.content == expected, "primitive output preserves schema order");
		ok &= expect(count_occurrences(primitive_result.content, "enabled = true") == 1,
		             "boolean option is emitted once");
		ok &= expect(count_occurrences(primitive_result.content, "count = 320") == 1,
		             "integer option is emitted once");
		ok &= expect(count_occurrences(primitive_result.content, "ratio = 1.25") == 1,
		             "floating-point option is emitted once");
		ok &= expect(count_occurrences(primitive_result.content, "path = none") == 1,
		             "string option is emitted once");
		ok &= expect(primitive_result.content.ends_with('\n'), "rendered config ends with newline");
		ok &= expect(!primitive_result.content.ends_with("\n\n"),
		             "rendered config has exactly one final newline");
	}

	const auto production_options = howdy::native::config_schema::runtime_config_options();
	const auto production_result =
	    howdy::native::config_template::render_default_config(production_options);
	ok &= expect(production_result.ok, "production schema renders");
	if (production_result.ok) {
		for (const auto &option : production_options) {
			ok &= expect(!option.description.empty(), "production option has description");
			ok &= expect(option.description != option.key, "description explains option");
		}
		for (const auto *const fallback : {"0.8845", "0.3", "0.6942", "1.25", "320", "75"}) {
			ok &= expect(production_result.content.contains(fallback),
			             std::string("production fallback is rendered: ") + fallback);
		}
		ok &= expect(!production_result.content.contains(".000000"),
		             "floating-point defaults do not use padded formatting");
		const auto repeated =
		    howdy::native::config_template::render_default_config(production_options);
		ok &= expect(repeated.ok && repeated.content == production_result.content,
		             "production rendering is deterministic");
	}

	const std::array empty_description = {
	    synthetic_option(OptionId::core_detection_notice, "one", "value", ValueType::boolean,
	                     howdy::native::config_schema::bool_default(false), ""),
	};
	ok &= rejects_rendering_error(empty_description, "empty or unsafe description");

	const std::array unsafe_section = {
	    synthetic_option(OptionId::core_detection_notice, "one]", "value", ValueType::boolean,
	                     howdy::native::config_schema::bool_default(false), "Boolean option."),
	};
	ok &= rejects_rendering_error(unsafe_section, "invalid or empty section");

	const std::array unsafe_key = {
	    synthetic_option(OptionId::core_detection_notice, "one", "value=other", ValueType::boolean,
	                     howdy::native::config_schema::bool_default(false), "Boolean option."),
	};
	ok &= rejects_rendering_error(unsafe_key, "invalid or empty key");

	for (const auto *const unsafe : {"line\nbreak", "value=other", "value # comment"}) {
		const std::array unsafe_string = {
		    synthetic_option(OptionId::core_detection_notice, "one", "value", ValueType::string,
		                     howdy::native::config_schema::string_default(unsafe),
		                     "String option."),
		};
		ok &= rejects_rendering_error(unsafe_string, "cannot serialize fallback");
	}

	const std::array repeated_section = {
	    synthetic_option(OptionId::core_detection_notice, "one", "first", ValueType::boolean,
	                     howdy::native::config_schema::bool_default(false), "First option."),
	    synthetic_option(OptionId::core_no_confirmation, "two", "second", ValueType::boolean,
	                     howdy::native::config_schema::bool_default(false), "Second option."),
	    synthetic_option(OptionId::core_abort_if_ssh, "one", "third", ValueType::boolean,
	                     howdy::native::config_schema::bool_default(false), "Third option."),
	};
	ok &= rejects_rendering_error(repeated_section, "section reused non-contiguously");

	const std::array<Option, 0> empty_schema = {};
	ok &= rejects_rendering_error(empty_schema, "schema has no options");

	return ok ? 0 : 1;
}
