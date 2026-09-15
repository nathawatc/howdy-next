#include "config/config_schema.hpp"
#include "support/face_metric.hpp"
#include "test_support.hpp"

#include <array>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {

	using howdy::native::config_schema::NumericRange;
	using howdy::native::config_schema::Option;
	using howdy::native::config_schema::OptionId;
	using howdy::native::config_schema::RuntimeDefault;
	using howdy::native::config_schema::SpecialRule;
	using howdy::native::config_schema::ValueType;
	using howdy::test::Expect;

	auto SyntheticOption(OptionId id, std::string_view section, std::string_view key,
	                     ValueType type, RuntimeDefault fallback) -> Option {
		return Option{.id           = id,
		              .section      = section,
		              .key          = key,
		              .type         = type,
		              .fallback     = fallback,
		              .range        = {},
		              .choices      = {},
		              .special_rule = SpecialRule::kNone,
		              .invalid_rule = "synthetic rule",
		              .description  = "Synthetic option."};
	}

	auto RangedOption(OptionId id, ValueType type, RuntimeDefault fallback, NumericRange range,
	                  SpecialRule special_rule = SpecialRule::kNone) -> Option {
		return Option{.id           = id,
		              .section      = "synthetic",
		              .key          = "value",
		              .type         = type,
		              .fallback     = fallback,
		              .range        = range,
		              .choices      = {},
		              .special_rule = special_rule,
		              .invalid_rule = "synthetic rule",
		              .description  = "Synthetic option."};
	}

	auto Accepts(std::span<const Option> options, std::string_view message) -> bool {
		return Expect(!howdy::native::config_schema::ValidateOptions(options).has_value(), message);
	}

	auto Rejects(std::span<const Option> options, std::string_view error_text) -> bool {
		const auto validation = howdy::native::config_schema::ValidateOptions(options);
		if (!validation.has_value()) {
			return Expect(false, "invalid schema is rejected");
		}
		return Expect(validation->contains(error_text),
		              "schema reports: " + std::string(error_text));
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	const auto production_options = howdy::native::config_schema::RuntimeConfigOptions();
	ok &= Expect(!howdy::native::config_schema::ValidateOptions(production_options).has_value(),
	             "production schema satisfies canonical invariants");

	const auto &sface_threshold_opt =
	    howdy::native::config_schema::RuntimeConfigOption(OptionId::kFaceSfaceThreshold);
	ok &= Expect(sface_threshold_opt.range.maximum == howdy::native::FaceMetricThresholdMaximum(),
	             "production sface_threshold range maximum matches face_metric_threshold_maximum");

	const std::array duplicate_ids = {
	    SyntheticOption(OptionId::kCoreDetectionNotice, "one", "first", ValueType::kBoolean,
	                    howdy::native::config_schema::BoolDefault(false)),
	    SyntheticOption(OptionId::kCoreDetectionNotice, "one", "second", ValueType::kBoolean,
	                    howdy::native::config_schema::BoolDefault(false)),
	};
	ok &= Rejects(duplicate_ids, "duplicate option id");

	const std::array duplicate_keys = {
	    SyntheticOption(OptionId::kCoreDetectionNotice, "one", "same", ValueType::kBoolean,
	                    howdy::native::config_schema::BoolDefault(false)),
	    SyntheticOption(OptionId::kCoreNoConfirmation, "one", "same", ValueType::kBoolean,
	                    howdy::native::config_schema::BoolDefault(false)),
	};
	ok &= Rejects(duplicate_keys, "duplicate section.key");

	const std::array duplicate_option_keys = {
	    SyntheticOption(OptionId::kCoreDetectionNotice, "one", "same", ValueType::kBoolean,
	                    howdy::native::config_schema::BoolDefault(false)),
	    SyntheticOption(OptionId::kCoreNoConfirmation, "two", "same", ValueType::kBoolean,
	                    howdy::native::config_schema::BoolDefault(false)),
	};
	ok &= Rejects(duplicate_option_keys, "duplicate option key");

	const std::array mismatched_fallback = {
	    SyntheticOption(OptionId::kCoreDetectionNotice, "one", "value", ValueType::kBoolean,
	                    howdy::native::config_schema::IntDefault(1)),
	};
	ok &= Rejects(mismatched_fallback, "fallback type mismatch");

	const std::array invalid_id = {
	    Option{.id           = static_cast<OptionId>(255),
	           .section      = "video",
	           .key          = "timeout",
	           .type         = ValueType::kInteger,
	           .fallback     = howdy::native::config_schema::IntDefault(5),
	           .range        = {.minimum = 1.0F, .maximum = 10.0F},
	           .invalid_rule = "synthetic rule",
	           .description  = "Synthetic option."},
	};
	ok &= Rejects(invalid_id, "option has invalid id");

	const std::array empty_section = {
	    SyntheticOption(OptionId::kCoreDetectionNotice, "", "value", ValueType::kBoolean,
	                    howdy::native::config_schema::BoolDefault(false)),
	};
	ok &= Rejects(empty_section, "invalid or empty section");

	const std::array empty_key = {
	    SyntheticOption(OptionId::kCoreDetectionNotice, "core", "", ValueType::kBoolean,
	                    howdy::native::config_schema::BoolDefault(false)),
	};
	ok &= Rejects(empty_key, "invalid or empty key");

	const std::array invalid_range = {
	    Option{.id           = OptionId::kVideoTimeout,
	           .section      = "video",
	           .key          = "timeout",
	           .type         = ValueType::kInteger,
	           .fallback     = howdy::native::config_schema::IntDefault(5),
	           .range        = {.minimum = 10.0F, .maximum = 5.0F},
	           .choices      = {},
	           .special_rule = SpecialRule::kNone,
	           .invalid_rule = "synthetic rule",
	           .description  = "Synthetic option."},
	};
	ok &= Rejects(invalid_range, "invalid numeric range");

	const std::array unrepresentable_integer_range = {
	    Option{.id           = OptionId::kVideoTimeout,
	           .section      = "video",
	           .key          = "timeout",
	           .type         = ValueType::kInteger,
	           .fallback     = howdy::native::config_schema::IntDefault(5),
	           .range        = {.minimum = 1.5F, .maximum = 10.0F},
	           .choices      = {},
	           .special_rule = SpecialRule::kNone,
	           .invalid_rule = "synthetic rule",
	           .description  = "Synthetic option."},
	};
	ok &= Rejects(unrepresentable_integer_range, "integer range is not representable");

	const std::array invalid_allowed_value = {
	    Option{.id           = OptionId::kVideoTimeout,
	           .section      = "video",
	           .key          = "timeout",
	           .type         = ValueType::kInteger,
	           .fallback     = howdy::native::config_schema::IntDefault(5),
	           .range        = {.minimum           = 1.0F,
	                            .maximum           = 10.0F,
	                            .has_allowed_value = true,
	                            .allowed_value     = 1.5F},
	           .choices      = {},
	           .special_rule = SpecialRule::kNone,
	           .invalid_rule = "synthetic rule",
	           .description  = "Synthetic option."},
	};
	ok &= Rejects(invalid_allowed_value, "allowed value is not representable");

	const NumericRange integer_range{.minimum = 1.0F, .maximum = 10.0F};
	const auto         integer_inside = std::array{
	    RangedOption(OptionId::kVideoTimeout, ValueType::kInteger,
	                 howdy::native::config_schema::IntDefault(5), integer_range),
	};
	ok &= Expect(!howdy::native::config_schema::ValidateOptions(integer_inside).has_value(),
	             "integer fallback inside range is valid");

	const auto integer_below = std::array{
	    RangedOption(OptionId::kVideoTimeout, ValueType::kInteger,
	                 howdy::native::config_schema::IntDefault(0), integer_range),
	};
	ok &= Rejects(integer_below, "integer fallback is outside range");

	const auto integer_above = std::array{
	    RangedOption(OptionId::kVideoTimeout, ValueType::kInteger,
	                 howdy::native::config_schema::IntDefault(11), integer_range),
	};
	ok &= Rejects(integer_above, "integer fallback is outside range");

	const NumericRange integer_sentinel_range{
	    .minimum           = 1.0F,
	    .maximum           = 10.0F,
	    .has_allowed_value = true,
	    .allowed_value     = -1.0F,
	};
	const auto integer_sentinel = std::array{
	    RangedOption(OptionId::kVideoFrameWidth, ValueType::kInteger,
	                 howdy::native::config_schema::IntDefault(-1), integer_sentinel_range),
	};
	ok &= Expect(!howdy::native::config_schema::ValidateOptions(integer_sentinel).has_value(),
	             "integer fallback allowed sentinel is valid");

	const NumericRange floating_range{.minimum = 2.0F, .maximum = 4.0F};
	for (const auto [fallback, message] : std::array{
	         std::pair{3.0F, "floating fallback inside range is valid"},
	         std::pair{2.0F, "floating fallback minimum is valid"},
	         std::pair{4.0F, "floating fallback maximum is valid"},
	     }) {
		const auto options = std::array{
		    RangedOption(OptionId::kFaceYunetScoreThreshold, ValueType::kFloatingPoint,
		                 howdy::native::config_schema::FloatDefault(fallback), floating_range),
		};
		ok &= Expect(!howdy::native::config_schema::ValidateOptions(options).has_value(), message);
	}

	for (const auto fallback : std::array{1.9F, 4.1F}) {
		const auto options = std::array{
		    RangedOption(OptionId::kFaceYunetScoreThreshold, ValueType::kFloatingPoint,
		                 howdy::native::config_schema::FloatDefault(fallback), floating_range),
		};
		ok &= Rejects(options, "floating-point fallback is outside range");
	}

	for (const auto fallback : std::array{
	         std::numeric_limits<float>::quiet_NaN(),
	         std::numeric_limits<float>::infinity(),
	         -std::numeric_limits<float>::infinity(),
	     }) {
		const auto options = std::array{
		    RangedOption(OptionId::kFaceYunetScoreThreshold, ValueType::kFloatingPoint,
		                 howdy::native::config_schema::FloatDefault(fallback), floating_range),
		};
		ok &= Rejects(options, "floating-point fallback is not finite");
	}

	const auto cosine_threshold_max =
	    howdy::native::GetFaceMetricPolicy(howdy::native::FaceMetric::kCosine)->threshold_maximum;
	const std::array<std::string_view, 1> cosine_choices = {
	    howdy::native::FaceMetricSpelling(howdy::native::FaceMetric::kCosine),
	};
	const auto sface_options = [&](float threshold) -> std::array<Option, 2> {
		return std::array{
		    Option{.id       = OptionId::kFaceSfaceMetric,
		           .section  = "face",
		           .key      = "sface_metric",
		           .type     = ValueType::kString,
		           .fallback = howdy::native::config_schema::StringDefault(
		               howdy::native::FaceMetricSpelling(howdy::native::FaceMetric::kCosine)),
		           .choices      = cosine_choices,
		           .special_rule = SpecialRule::kNone,
		           .invalid_rule = "synthetic rule",
		           .description  = "Synthetic option."},
		    Option{
		        .id       = OptionId::kFaceSfaceThreshold,
		        .section  = "face",
		        .key      = "sface_threshold",
		        .type     = ValueType::kFloatingPoint,
		        .fallback = howdy::native::config_schema::FloatDefault(threshold),
		        .range = {.minimum = 0.0F, .maximum = howdy::native::FaceMetricThresholdMaximum()},
		        .special_rule = SpecialRule::kSfaceThreshold,
		        .invalid_rule = "synthetic rule",
		        .description  = "Synthetic option."},
		};
	};
	const auto valid_sface = sface_options(cosine_threshold_max);
	ok &= Accepts(valid_sface, "cosine SFace fallback at effective maximum is valid");
	const auto invalid_sface = sface_options(cosine_threshold_max + 0.1F);
	ok &= Rejects(invalid_sface, "sface threshold fallback exceeds cosine range");

	const std::array<std::string_view, 3> duplicate_choices        = {"cosine", "l2", "cosine"};
	const std::array                      duplicate_choice_options = {
	    Option{.id           = OptionId::kFaceSfaceMetric,
	           .section      = "face",
	           .key          = "sface_metric",
	           .type         = ValueType::kString,
	           .fallback     = howdy::native::config_schema::StringDefault("cosine"),
	           .choices      = duplicate_choices,
	           .special_rule = SpecialRule::kNone,
	           .invalid_rule = "synthetic rule",
	           .description  = "Synthetic option."},
	};
	ok &= Rejects(duplicate_choice_options, "duplicate choice");

	const std::array<std::string_view, 1> empty_choice         = {""};
	const std::array                      empty_choice_options = {
	    Option{.id           = OptionId::kFaceSfaceMetric,
	           .section      = "face",
	           .key          = "sface_metric",
	           .type         = ValueType::kString,
	           .fallback     = howdy::native::config_schema::StringDefault(""),
	           .choices      = empty_choice,
	           .special_rule = SpecialRule::kNone,
	           .invalid_rule = "synthetic rule",
	           .description  = "Synthetic option."},
	};
	ok &= Rejects(empty_choice_options, "choice is empty");

	const std::array<std::string_view, 1> incompatible_choices        = {"true"};
	const std::array                      incompatible_choice_options = {
	    Option{.id           = OptionId::kCoreDetectionNotice,
	           .section      = "core",
	           .key          = "detection_notice",
	           .type         = ValueType::kBoolean,
	           .fallback     = howdy::native::config_schema::BoolDefault(false),
	           .choices      = incompatible_choices,
	           .special_rule = SpecialRule::kNone,
	           .invalid_rule = "synthetic rule",
	           .description  = "Synthetic option."},
	};
	ok &= Rejects(incompatible_choice_options, "choices are incompatible with option type");

	const std::array<std::string_view, 2> fallback_not_in_choices       = {"cosine", "l2"};
	const std::array                      incompatible_fallback_options = {
	    Option{.id           = OptionId::kFaceSfaceMetric,
	           .section      = "face",
	           .key          = "sface_metric",
	           .type         = ValueType::kString,
	           .fallback     = howdy::native::config_schema::StringDefault("l2norm"),
	           .choices      = fallback_not_in_choices,
	           .special_rule = SpecialRule::kNone,
	           .invalid_rule = "synthetic rule",
	           .description  = "Synthetic option."},
	};
	ok &= Rejects(incompatible_fallback_options, "fallback is not one of choices");

	const std::array<std::string_view, 1> device_path_choices   = {"none"};
	const std::array                      free_form_device_path = {
	    Option{.id           = OptionId::kVideoDevicePath,
	           .section      = "video",
	           .key          = "device_path",
	           .type         = ValueType::kString,
	           .fallback     = howdy::native::config_schema::StringDefault("/dev/video0"),
	           .choices      = device_path_choices,
	           .special_rule = SpecialRule::kDevicePath,
	           .invalid_rule = "synthetic rule",
	           .description  = "Synthetic option."},
	};
	ok &= Expect(!howdy::native::config_schema::ValidateOptions(free_form_device_path).has_value(),
	             "device path fallback remains free-form");

	const std::array incompatible_special_rule = {
	    Option{.id           = OptionId::kCoreDetectionNotice,
	           .section      = "core",
	           .key          = "detection_notice",
	           .type         = ValueType::kInteger,
	           .fallback     = howdy::native::config_schema::IntDefault(1),
	           .special_rule = SpecialRule::kDevicePath,
	           .invalid_rule = "synthetic rule",
	           .description  = "Synthetic option."},
	};
	ok &= Rejects(incompatible_special_rule, "special rule/type mismatch");

	const std::array unknown_special_rule = {
	    Option{.id           = OptionId::kCoreDetectionNotice,
	           .section      = "core",
	           .key          = "detection_notice",
	           .type         = ValueType::kBoolean,
	           .fallback     = howdy::native::config_schema::BoolDefault(false),
	           .special_rule = static_cast<SpecialRule>(255),
	           .invalid_rule = "synthetic rule",
	           .description  = "Synthetic option."},
	};
	ok &= Rejects(unknown_special_rule, "unknown special rule");

	for (const auto &spelling : howdy::native::config_schema::kAcceptedBooleanSpellings) {
		ok &= Expect(howdy::native::config_schema::IsAcceptedBooleanText(spelling),
		             "lowercase boolean spelling is accepted: " + std::string(spelling));
		std::string upper(spelling);
		for (char &ch : upper) {
			ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
		}
		ok &= Expect(howdy::native::config_schema::IsAcceptedBooleanText(upper),
		             "uppercase boolean spelling is accepted: " + upper);
	}
	for (const auto *const invalid_bool :
	     {"maybe", "2", "-1", "enable", "none", "", "true1", "00"}) {
		ok &= Expect(!howdy::native::config_schema::IsAcceptedBooleanText(invalid_bool),
		             "invalid boolean spelling is rejected: " + std::string(invalid_bool));
	}

	ok &= Expect(howdy::native::config_schema::FormatIntegerValue(0) == "0",
	             "format_integer_value formats zero");
	ok &= Expect(howdy::native::config_schema::FormatIntegerValue(-1) == "-1",
	             "format_integer_value formats negative sentinel");
	ok &= Expect(howdy::native::config_schema::FormatIntegerValue(320) == "320",
	             "format_integer_value formats positive integer");

	ok &= Expect(howdy::native::config_schema::FormatFloatingPointValue(0.0F) == "0",
	             "format_floating_point_value formats zero");
	ok &= Expect(howdy::native::config_schema::FormatFloatingPointValue(1.25F) == "1.25",
	             "format_floating_point_value formats 1.25");
	ok &= Expect(howdy::native::config_schema::FormatFloatingPointValue(0.6942F) == "0.6942",
	             "format_floating_point_value formats 0.6942");
	ok &= Expect(!howdy::native::config_schema::FormatFloatingPointValue(
	                  std::numeric_limits<float>::quiet_NaN())
	                  .has_value(),
	             "format_floating_point_value rejects NaN");

	const auto bool_opt =
	    SyntheticOption(OptionId::kCoreDetectionNotice, "core", "test", ValueType::kBoolean,
	                    howdy::native::config_schema::BoolDefault(true));
	ok &= Expect(howdy::native::config_schema::FormatFallbackValue(bool_opt) == "true",
	             "format_fallback_value formats boolean true");
	const auto int_opt =
	    SyntheticOption(OptionId::kVideoTimeout, "video", "test", ValueType::kInteger,
	                    howdy::native::config_schema::IntDefault(4));
	ok &= Expect(howdy::native::config_schema::FormatFallbackValue(int_opt) == "4",
	             "format_fallback_value formats integer fallback");
	const auto float_opt =
	    SyntheticOption(OptionId::kFaceSfaceThreshold, "face", "test", ValueType::kFloatingPoint,
	                    howdy::native::config_schema::FloatDefault(0.6942F));
	ok &= Expect(howdy::native::config_schema::FormatFallbackValue(float_opt) == "0.6942",
	             "format_fallback_value formats float fallback");
	const auto string_opt =
	    SyntheticOption(OptionId::kVideoDevicePath, "video", "test", ValueType::kString,
	                    howdy::native::config_schema::StringDefault("none"));
	ok &= Expect(howdy::native::config_schema::FormatFallbackValue(string_opt) == "none",
	             "format_fallback_value formats string fallback");

	return ok ? 0 : 1;
}
