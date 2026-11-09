#pragma once

#include "config/config_schema.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace howdy::native {

	// Schema-checked view of config.toml. Parsing (Rust howdy-config over FFI) rejects TOML syntax
	// errors and non-table shapes; this class then rejects unknown tables or keys and values whose
	// TOML type does not match the schema type, and keeps each accepted value with its schema type.
	// An empty string counts as unset.
	class ConfigDocument {
	public:
		[[nodiscard]] static auto FromContent(std::string_view content) -> ConfigDocument;

		[[nodiscard]] auto Ok() const -> bool;
		[[nodiscard]] auto Error() const -> const std::string &;
		// The typed value, or null when the option is unset.
		[[nodiscard]] auto Value(std::string_view section, std::string_view name) const
		    -> const config_schema::ConfigValue *;
		[[nodiscard]] auto GetBool(std::string_view section, std::string_view name,
		                           bool fallback) const -> bool;
		[[nodiscard]] auto GetInt(std::string_view section, std::string_view name,
		                          int fallback) const -> int;
		[[nodiscard]] auto GetFloat(std::string_view section, std::string_view name,
		                            float fallback) const -> float;
		[[nodiscard]] auto GetString(std::string_view section, std::string_view name,
		                             const std::string &fallback) const -> std::string;

	private:
		std::map<config_schema::OptionId, config_schema::ConfigValue> values_;
		std::string                                                   error_;
		bool                                                          ok_ = false;
	};

	struct ConfigEditResult {
		bool        ok = false;
		std::string content;
		std::string error;
	};

	// Writes `option = value` into config.toml text, keeping comments and the layout around the
	// value (line endings become LF and a byte-order mark is dropped). A missing key is
	// added to its table and a missing table is appended. Only TOML structure is checked; callers
	// validate the result with ConfigDocument.
	[[nodiscard]] auto SetConfigValue(std::string_view content, const config_schema::Option &option,
	                                  const config_schema::ConfigValue &value) -> ConfigEditResult;

	// Whether `text` may appear in a config string value: printable ASCII only. The rule is
	// Rust's (howdy_config::is_safe_text).
	[[nodiscard]] auto IsSafeConfigText(std::string_view text) -> bool;

	// Converts `howdy set` text to the option's type. Booleans and numbers use TOML literal
	// syntax (`true`, `+5`, `1_000`, `0.5`); strings are taken as given and must be safe text.
	// Only syntax and type are checked; ranges and choices are validated with the whole config.
	[[nodiscard]] auto ParseOptionValueText(const config_schema::Option &option,
	                                        std::string_view             text)
	    -> std::optional<config_schema::ConfigValue>;

}  // namespace howdy::native
