#include "../../src/config/config_reader/internal.hpp"
#include "../../src/config/config_validation/internal.hpp"
#include "config/config_schema.hpp"
#include "config/config_template.hpp"
#include "test_support.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <locale>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

	using howdy::native::config_schema::Option;
	using howdy::native::config_schema::OptionId;
	using howdy::native::config_schema::RuntimeDefault;
	using howdy::native::config_schema::ValueType;
	using howdy::test::Expect;
	using howdy::test::WriteFile;

	class CommaDecimalNumpunct final : public std::numpunct<char> {
	protected:
		auto do_decimal_point() const -> char override {
			return ',';
		}
	};

	auto SyntheticOption(OptionId id, std::string_view section, std::string_view key,
	                     ValueType type, RuntimeDefault fallback, std::string_view description)
	    -> Option {
		return Option{.id           = id,
		              .section      = section,
		              .key          = key,
		              .type         = type,
		              .fallback     = fallback,
		              .range        = {},
		              .choices      = {},
		              .special_rule = howdy::native::config_schema::SpecialRule::kNone,
		              .invalid_rule = "synthetic rule",
		              .description  = description};
	}

	auto Render(const auto &options) -> howdy::native::config_template::ConfigTemplateRenderResult {
		return howdy::native::config_template::RenderDefaultConfig(
		    std::span<const Option>(options));
	}

	auto RejectsRenderingError(const auto &options, std::string_view error_text) -> bool {
		const auto result = Render(options);
		return Expect(!result.ok, "rendering constraint is rejected") &&
		       Expect(result.error.contains(error_text),
		              "renderer reports: " + std::string(error_text));
	}

	auto CountOccurrences(std::string_view text, std::string_view needle) -> std::size_t {
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
	    SyntheticOption(OptionId::kCoreDetectionNotice, "alpha", "enabled", ValueType::kBoolean,
	                    howdy::native::config_schema::BoolDefault(true), "Enable synthetic mode."),
	    SyntheticOption(OptionId::kCoreNoConfirmation, "alpha", "count", ValueType::kInteger,
	                    howdy::native::config_schema::IntDefault(320), "Number of samples to use."),
	    SyntheticOption(OptionId::kCoreAbortIfSsh, "beta", "ratio", ValueType::kFloatingPoint,
	                    howdy::native::config_schema::FloatDefault(1.25F),
	                    "Scaling ratio for synthetic mode."),
	    SyntheticOption(OptionId::kCoreAbortIfLidClosed, "beta", "path", ValueType::kString,
	                    howdy::native::config_schema::StringDefault("none"),
	                    "Device path used by synthetic mode."),
	};
	const auto primitive_result = Render(primitive_options);
	ok &= Expect(primitive_result.ok, "primitive schema renders");
	if (primitive_result.ok) {
		const auto previous_locale =
		    std::locale::global(std::locale(std::locale::classic(), new CommaDecimalNumpunct));
		const auto locale_result = Render(primitive_options);
		std::locale::global(previous_locale);
		ok &= Expect(locale_result.ok && locale_result.content.contains("ratio = 1.25\n"),
		             "floating-point rendering ignores comma decimal locale");

		const std::string expected = "# See howdy.ini(5) for configuration options.\n\n"
		                             "[alpha]\n"
		                             "enabled = true\n"
		                             "count = 320\n\n"
		                             "[beta]\n"
		                             "ratio = 1.25\n"
		                             "path = none\n";
		ok &=
		    Expect(primitive_result.content == expected, "primitive output preserves schema order");
		ok &= Expect(CountOccurrences(primitive_result.content, "#") == 1,
		             "primitive output contains exactly one comment");
		ok &= Expect(CountOccurrences(primitive_result.content,
		                              "# See howdy.ini(5) for configuration options.") == 1,
		             "primitive output has short header comment");
		ok &= Expect(!primitive_result.content.contains("# Enable synthetic mode."),
		             "primitive output does not contain description comments");
		ok &= Expect(!primitive_result.content.contains("# Number of samples to use."),
		             "primitive output does not contain description comments");
		ok &= Expect(CountOccurrences(primitive_result.content, "enabled = true") == 1,
		             "boolean option is emitted once");
		ok &= Expect(CountOccurrences(primitive_result.content, "count = 320") == 1,
		             "integer option is emitted once");
		ok &= Expect(CountOccurrences(primitive_result.content, "ratio = 1.25") == 1,
		             "floating-point option is emitted once");
		ok &= Expect(CountOccurrences(primitive_result.content, "path = none") == 1,
		             "string option is emitted once");
		ok &= Expect(primitive_result.content.ends_with('\n'), "rendered config ends with newline");
		ok &= Expect(!primitive_result.content.ends_with("\n\n"),
		             "rendered config has exactly one final newline");
	}

	const auto production_options = howdy::native::config_schema::RuntimeConfigOptions();
	const auto production_result =
	    howdy::native::config_template::RenderDefaultConfig(production_options);
	ok &= Expect(production_result.ok, "production schema renders");
	if (production_result.ok) {
		ok &= Expect(production_result.content.starts_with(
		                 "# See howdy.ini(5) for configuration options.\n\n"),
		             "production config starts with short header comment");
		ok &= Expect(CountOccurrences(production_result.content, "#") == 1,
		             "production config contains only the single header comment");
		for (const auto &option : production_options) {
			ok &= Expect(!option.description.empty(), "production option has description");
			ok &= Expect(option.description != option.key, "description explains option");
			ok &=
			    Expect(!production_result.content.contains("# " + std::string(option.description)),
			           "production config does not contain per-option description comment: " +
			               std::string(option.key));
			const auto canonical_fallback =
			    howdy::native::config_schema::FormatFallbackValue(option);
			ok &= Expect(canonical_fallback.has_value() &&
			                 production_result.content.contains(std::string(option.key) + " = " +
			                                                    *canonical_fallback + "\n"),
			             "rendered config matches canonical schema fallback formatting for " +
			                 std::string(option.key));
		}
		for (const auto *const fallback : {"0.8845", "0.3", "0.6942", "1.25", "320", "75"}) {
			ok &= Expect(production_result.content.contains(fallback),
			             std::string("production fallback is rendered: ") + fallback);
		}
		ok &= Expect(!production_result.content.contains(".000000"),
		             "floating-point defaults do not use padded formatting");
		const auto repeated =
		    howdy::native::config_template::RenderDefaultConfig(production_options);
		ok &= Expect(repeated.ok && repeated.content == production_result.content,
		             "production rendering is deterministic");

		const auto temp_config_path =
		    std::filesystem::temp_directory_path() /
		    ("howdy-rendered-config-test-" + std::to_string(getpid()) + ".ini");
		std::error_code ec;
		std::filesystem::remove(temp_config_path, ec);
		ok &= Expect(WriteFile(temp_config_path, production_result.content),
		             "write generated production config to temp file");
		const howdy::native::ConfigReader reader(temp_config_path.string());
		ok &= Expect(reader.Ok(), "rendered production config parses without INI syntax errors");
		const auto validation_error = howdy::native::ValidateRuntimeConfig(reader);
		ok &= Expect(!validation_error.has_value(),
		             "rendered production config passes full canonical runtime validation");
		std::filesystem::remove(temp_config_path, ec);
	}

	const std::array empty_description = {
	    SyntheticOption(OptionId::kCoreDetectionNotice, "one", "value", ValueType::kBoolean,
	                    howdy::native::config_schema::BoolDefault(false), ""),
	};
	ok &= RejectsRenderingError(empty_description, "empty or unsafe description");

	const std::array unsafe_section = {
	    SyntheticOption(OptionId::kCoreDetectionNotice, "one]", "value", ValueType::kBoolean,
	                    howdy::native::config_schema::BoolDefault(false), "Boolean option."),
	};
	ok &= RejectsRenderingError(unsafe_section, "invalid or empty section");

	const std::array unsafe_key = {
	    SyntheticOption(OptionId::kCoreDetectionNotice, "one", "value=other", ValueType::kBoolean,
	                    howdy::native::config_schema::BoolDefault(false), "Boolean option."),
	};
	ok &= RejectsRenderingError(unsafe_key, "invalid or empty key");

	for (const auto *const unsafe : {"line\nbreak", "value=other", "value # comment"}) {
		const std::array unsafe_string = {
		    SyntheticOption(OptionId::kCoreDetectionNotice, "one", "value", ValueType::kString,
		                    howdy::native::config_schema::StringDefault(unsafe), "String option."),
		};
		ok &= RejectsRenderingError(unsafe_string, "cannot serialize fallback");
	}

	const std::array repeated_section = {
	    SyntheticOption(OptionId::kCoreDetectionNotice, "one", "first", ValueType::kBoolean,
	                    howdy::native::config_schema::BoolDefault(false), "First option."),
	    SyntheticOption(OptionId::kCoreNoConfirmation, "two", "second", ValueType::kBoolean,
	                    howdy::native::config_schema::BoolDefault(false), "Second option."),
	    SyntheticOption(OptionId::kCoreAbortIfSsh, "one", "third", ValueType::kBoolean,
	                    howdy::native::config_schema::BoolDefault(false), "Third option."),
	};
	ok &= RejectsRenderingError(repeated_section, "section reused non-contiguously");

	const std::array<Option, 0> empty_schema = {};
	ok &= RejectsRenderingError(empty_schema, "schema has no options");

	return ok ? 0 : 1;
}
