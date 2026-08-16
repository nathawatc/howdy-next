#include "config/config_schema.hpp"
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
	using howdy::test::expect;

	auto synthetic_option(OptionId id, std::string_view section, std::string_view key,
	                      ValueType type, RuntimeDefault fallback) -> Option {
		return Option{.id           = id,
		              .section      = section,
		              .key          = key,
		              .type         = type,
		              .fallback     = fallback,
		              .range        = {},
		              .choices      = {},
		              .special_rule = SpecialRule::none,
		              .invalid_rule = "synthetic rule",
		              .description  = "Synthetic option."};
	}

	auto ranged_option(OptionId id, ValueType type, RuntimeDefault fallback, NumericRange range,
	                   SpecialRule special_rule = SpecialRule::none) -> Option {
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

	auto accepts(std::span<const Option> options, std::string_view message) -> bool {
		return expect(!howdy::native::config_schema::validate_options(options).has_value(),
		              message);
	}

	auto rejects(std::span<const Option> options, std::string_view error_text) -> bool {
		const auto validation = howdy::native::config_schema::validate_options(options);
		if (!validation.has_value()) {
			return expect(false, "invalid schema is rejected");
		}
		return expect(validation->contains(error_text),
		              "schema reports: " + std::string(error_text));
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	const auto production_options = howdy::native::config_schema::runtime_config_options();
	ok &= expect(!howdy::native::config_schema::validate_options(production_options).has_value(),
	             "production schema satisfies canonical invariants");

	const std::array duplicate_ids = {
	    synthetic_option(OptionId::core_detection_notice, "one", "first", ValueType::boolean,
	                     howdy::native::config_schema::bool_default(false)),
	    synthetic_option(OptionId::core_detection_notice, "one", "second", ValueType::boolean,
	                     howdy::native::config_schema::bool_default(false)),
	};
	ok &= rejects(duplicate_ids, "duplicate option id");

	const std::array duplicate_keys = {
	    synthetic_option(OptionId::core_detection_notice, "one", "same", ValueType::boolean,
	                     howdy::native::config_schema::bool_default(false)),
	    synthetic_option(OptionId::core_no_confirmation, "one", "same", ValueType::boolean,
	                     howdy::native::config_schema::bool_default(false)),
	};
	ok &= rejects(duplicate_keys, "duplicate section.key");

	const std::array duplicate_option_keys = {
	    synthetic_option(OptionId::core_detection_notice, "one", "same", ValueType::boolean,
	                     howdy::native::config_schema::bool_default(false)),
	    synthetic_option(OptionId::core_no_confirmation, "two", "same", ValueType::boolean,
	                     howdy::native::config_schema::bool_default(false)),
	};
	ok &= rejects(duplicate_option_keys, "duplicate option key");

	const std::array mismatched_fallback = {
	    synthetic_option(OptionId::core_detection_notice, "one", "value", ValueType::boolean,
	                     howdy::native::config_schema::int_default(1)),
	};
	ok &= rejects(mismatched_fallback, "fallback type mismatch");

	const std::array invalid_id = {
	    Option{.id           = static_cast<OptionId>(255),
	           .section      = "video",
	           .key          = "timeout",
	           .type         = ValueType::integer,
	           .fallback     = howdy::native::config_schema::int_default(5),
	           .range        = {.minimum = 1.0F, .maximum = 10.0F},
	           .invalid_rule = "synthetic rule",
	           .description  = "Synthetic option."},
	};
	ok &= rejects(invalid_id, "option has invalid id");

	const std::array empty_section = {
	    synthetic_option(OptionId::core_detection_notice, "", "value", ValueType::boolean,
	                     howdy::native::config_schema::bool_default(false)),
	};
	ok &= rejects(empty_section, "invalid or empty section");

	const std::array empty_key = {
	    synthetic_option(OptionId::core_detection_notice, "core", "", ValueType::boolean,
	                     howdy::native::config_schema::bool_default(false)),
	};
	ok &= rejects(empty_key, "invalid or empty key");

	const std::array invalid_range = {
	    Option{.id           = OptionId::video_timeout,
	           .section      = "video",
	           .key          = "timeout",
	           .type         = ValueType::integer,
	           .fallback     = howdy::native::config_schema::int_default(5),
	           .range        = {.minimum = 10.0F, .maximum = 5.0F},
	           .choices      = {},
	           .special_rule = SpecialRule::none,
	           .invalid_rule = "synthetic rule",
	           .description  = "Synthetic option."},
	};
	ok &= rejects(invalid_range, "invalid numeric range");

	const std::array unrepresentable_integer_range = {
	    Option{.id           = OptionId::video_timeout,
	           .section      = "video",
	           .key          = "timeout",
	           .type         = ValueType::integer,
	           .fallback     = howdy::native::config_schema::int_default(5),
	           .range        = {.minimum = 1.5F, .maximum = 10.0F},
	           .choices      = {},
	           .special_rule = SpecialRule::none,
	           .invalid_rule = "synthetic rule",
	           .description  = "Synthetic option."},
	};
	ok &= rejects(unrepresentable_integer_range, "integer range is not representable");

	const std::array invalid_allowed_value = {
	    Option{.id           = OptionId::video_timeout,
	           .section      = "video",
	           .key          = "timeout",
	           .type         = ValueType::integer,
	           .fallback     = howdy::native::config_schema::int_default(5),
	           .range        = {.minimum           = 1.0F,
	                            .maximum           = 10.0F,
	                            .has_allowed_value = true,
	                            .allowed_value     = 1.5F},
	           .choices      = {},
	           .special_rule = SpecialRule::none,
	           .invalid_rule = "synthetic rule",
	           .description  = "Synthetic option."},
	};
	ok &= rejects(invalid_allowed_value, "allowed value is not representable");

	const NumericRange integer_range{.minimum = 1.0F, .maximum = 10.0F};
	const auto         integer_inside = std::array{
	    ranged_option(OptionId::video_timeout, ValueType::integer,
	                  howdy::native::config_schema::int_default(5), integer_range),
	};
	ok &= expect(!howdy::native::config_schema::validate_options(integer_inside).has_value(),
	             "integer fallback inside range is valid");

	const auto integer_below = std::array{
	    ranged_option(OptionId::video_timeout, ValueType::integer,
	                  howdy::native::config_schema::int_default(0), integer_range),
	};
	ok &= rejects(integer_below, "integer fallback is outside range");

	const auto integer_above = std::array{
	    ranged_option(OptionId::video_timeout, ValueType::integer,
	                  howdy::native::config_schema::int_default(11), integer_range),
	};
	ok &= rejects(integer_above, "integer fallback is outside range");

	const NumericRange integer_sentinel_range{
	    .minimum           = 1.0F,
	    .maximum           = 10.0F,
	    .has_allowed_value = true,
	    .allowed_value     = -1.0F,
	};
	const auto integer_sentinel = std::array{
	    ranged_option(OptionId::video_frame_width, ValueType::integer,
	                  howdy::native::config_schema::int_default(-1), integer_sentinel_range),
	};
	ok &= expect(!howdy::native::config_schema::validate_options(integer_sentinel).has_value(),
	             "integer fallback allowed sentinel is valid");

	const NumericRange floating_range{.minimum = 2.0F, .maximum = 4.0F};
	for (const auto [fallback, message] : std::array{
	         std::pair{3.0F, "floating fallback inside range is valid"},
	         std::pair{2.0F, "floating fallback minimum is valid"},
	         std::pair{4.0F, "floating fallback maximum is valid"},
	     }) {
		const auto options = std::array{
		    ranged_option(OptionId::face_yunet_score_threshold, ValueType::floating_point,
		                  howdy::native::config_schema::float_default(fallback), floating_range),
		};
		ok &= expect(!howdy::native::config_schema::validate_options(options).has_value(), message);
	}

	for (const auto fallback : std::array{1.9F, 4.1F}) {
		const auto options = std::array{
		    ranged_option(OptionId::face_yunet_score_threshold, ValueType::floating_point,
		                  howdy::native::config_schema::float_default(fallback), floating_range),
		};
		ok &= rejects(options, "floating-point fallback is outside range");
	}

	for (const auto fallback : std::array{
	         std::numeric_limits<float>::quiet_NaN(),
	         std::numeric_limits<float>::infinity(),
	         -std::numeric_limits<float>::infinity(),
	     }) {
		const auto options = std::array{
		    ranged_option(OptionId::face_yunet_score_threshold, ValueType::floating_point,
		                  howdy::native::config_schema::float_default(fallback), floating_range),
		};
		ok &= rejects(options, "floating-point fallback is not finite");
	}

	const std::array<std::string_view, 1> cosine_choices = {
	    howdy::native::config_schema::sface_cosine_metric,
	};
	const auto sface_options = [&](float threshold) -> std::array<Option, 2> {
		return std::array{
		    Option{.id       = OptionId::face_sface_metric,
		           .section  = "face",
		           .key      = "sface_metric",
		           .type     = ValueType::string,
		           .fallback = howdy::native::config_schema::string_default(
		               howdy::native::config_schema::sface_cosine_metric),
		           .choices      = cosine_choices,
		           .special_rule = SpecialRule::none,
		           .invalid_rule = "synthetic rule",
		           .description  = "Synthetic option."},
		    Option{.id           = OptionId::face_sface_threshold,
		           .section      = "face",
		           .key          = "sface_threshold",
		           .type         = ValueType::floating_point,
		           .fallback     = howdy::native::config_schema::float_default(threshold),
		           .range        = {.minimum = 0.0F, .maximum = 4.0F},
		           .special_rule = SpecialRule::sface_threshold,
		           .invalid_rule = "synthetic rule",
		           .description  = "Synthetic option."},
		};
	};
	const auto valid_sface =
	    sface_options(howdy::native::config_schema::sface_cosine_threshold_maximum);
	ok &= accepts(valid_sface, "cosine SFace fallback at effective maximum is valid");
	const auto invalid_sface =
	    sface_options(howdy::native::config_schema::sface_cosine_threshold_maximum + 0.1F);
	ok &= rejects(invalid_sface, "sface threshold fallback exceeds cosine range");

	const std::array<std::string_view, 3> duplicate_choices        = {"cosine", "l2", "cosine"};
	const std::array                      duplicate_choice_options = {
	    Option{.id           = OptionId::face_sface_metric,
	           .section      = "face",
	           .key          = "sface_metric",
	           .type         = ValueType::string,
	           .fallback     = howdy::native::config_schema::string_default("cosine"),
	           .choices      = duplicate_choices,
	           .special_rule = SpecialRule::none,
	           .invalid_rule = "synthetic rule",
	           .description  = "Synthetic option."},
	};
	ok &= rejects(duplicate_choice_options, "duplicate choice");

	const std::array<std::string_view, 1> empty_choice         = {""};
	const std::array                      empty_choice_options = {
	    Option{.id           = OptionId::face_sface_metric,
	           .section      = "face",
	           .key          = "sface_metric",
	           .type         = ValueType::string,
	           .fallback     = howdy::native::config_schema::string_default(""),
	           .choices      = empty_choice,
	           .special_rule = SpecialRule::none,
	           .invalid_rule = "synthetic rule",
	           .description  = "Synthetic option."},
	};
	ok &= rejects(empty_choice_options, "choice is empty");

	const std::array<std::string_view, 1> incompatible_choices        = {"true"};
	const std::array                      incompatible_choice_options = {
	    Option{.id           = OptionId::core_detection_notice,
	           .section      = "core",
	           .key          = "detection_notice",
	           .type         = ValueType::boolean,
	           .fallback     = howdy::native::config_schema::bool_default(false),
	           .choices      = incompatible_choices,
	           .special_rule = SpecialRule::none,
	           .invalid_rule = "synthetic rule",
	           .description  = "Synthetic option."},
	};
	ok &= rejects(incompatible_choice_options, "choices are incompatible with option type");

	const std::array<std::string_view, 2> fallback_not_in_choices       = {"cosine", "l2"};
	const std::array                      incompatible_fallback_options = {
	    Option{.id           = OptionId::face_sface_metric,
	           .section      = "face",
	           .key          = "sface_metric",
	           .type         = ValueType::string,
	           .fallback     = howdy::native::config_schema::string_default("l2norm"),
	           .choices      = fallback_not_in_choices,
	           .special_rule = SpecialRule::none,
	           .invalid_rule = "synthetic rule",
	           .description  = "Synthetic option."},
	};
	ok &= rejects(incompatible_fallback_options, "fallback is not one of choices");

	const std::array<std::string_view, 1> device_path_choices   = {"none"};
	const std::array                      free_form_device_path = {
	    Option{.id           = OptionId::video_device_path,
	           .section      = "video",
	           .key          = "device_path",
	           .type         = ValueType::string,
	           .fallback     = howdy::native::config_schema::string_default("/dev/video0"),
	           .choices      = device_path_choices,
	           .special_rule = SpecialRule::device_path,
	           .invalid_rule = "synthetic rule",
	           .description  = "Synthetic option."},
	};
	ok &= expect(!howdy::native::config_schema::validate_options(free_form_device_path).has_value(),
	             "device path fallback remains free-form");

	const std::array incompatible_special_rule = {
	    Option{.id           = OptionId::core_detection_notice,
	           .section      = "core",
	           .key          = "detection_notice",
	           .type         = ValueType::integer,
	           .fallback     = howdy::native::config_schema::int_default(1),
	           .special_rule = SpecialRule::device_path,
	           .invalid_rule = "synthetic rule",
	           .description  = "Synthetic option."},
	};
	ok &= rejects(incompatible_special_rule, "special rule/type mismatch");

	const std::array unknown_special_rule = {
	    Option{.id           = OptionId::core_detection_notice,
	           .section      = "core",
	           .key          = "detection_notice",
	           .type         = ValueType::boolean,
	           .fallback     = howdy::native::config_schema::bool_default(false),
	           .special_rule = static_cast<SpecialRule>(255),
	           .invalid_rule = "synthetic rule",
	           .description  = "Synthetic option."},
	};
	ok &= rejects(unknown_special_rule, "unknown special rule");

	return ok ? 0 : 1;
}
