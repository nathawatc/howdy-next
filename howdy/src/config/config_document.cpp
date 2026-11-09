#include "config/config_limits.hpp"
#include "config/config_schema.hpp"
#include "config_document/internal.hpp"

#include "howdy_ffi.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace howdy::native {

	// The C++ read limit and the limit Rust enforces are one value.
	static_assert(kMaxConfigFileSize == HOWDY_CONFIG_MAX_BYTES);

	namespace {

		struct DocumentDeleter {
			void operator()(HowdyConfigDocument *document) const noexcept {
				howdy_config_document_free(document);
			}
		};

		struct EditResultDeleter {
			void operator()(HowdyConfigEditResult *result) const noexcept {
				howdy_config_edit_result_free(result);
			}
		};

		using DocumentHandle   = std::unique_ptr<HowdyConfigDocument, DocumentDeleter>;
		using EditResultHandle = std::unique_ptr<HowdyConfigEditResult, EditResultDeleter>;

		auto View(std::string_view text) -> HowdyStr {
			return {.ptr = text.data(), .len = text.size()};
		}

		auto Text(HowdyStr text) -> std::string_view {
			return text.len == 0 ? std::string_view{} : std::string_view(text.ptr, text.len);
		}

		auto PositionPrefix(std::size_t line, std::size_t column) -> std::string {
			if (line == 0) {
				return {};
			}
			return "line " + std::to_string(line) + ", column " + std::to_string(column) + ": ";
		}

		auto FfiFailureMessage(HowdyFfiStatus status) -> std::string_view {
			switch (status) {
				case HOWDY_FFI_STATUS_INVALID_UTF8:
					return "config is not valid UTF-8";
				case HOWDY_FFI_STATUS_TOO_LARGE:
					return "config exceeds maximum size";
				default:
					return "config could not be processed";
			}
		}

		auto ErrorMessage(const HowdyConfigError &error) -> std::string {
			const std::string_view kind =
			    error.kind == HOWDY_CONFIG_ERROR_KIND_SYNTAX ? "not valid TOML: " : "";
			const auto message = Text(error.message);
			return PositionPrefix(error.line, error.column) + std::string(kind) +
			       (message.empty() ? std::string("config is invalid") : std::string(message));
		}

		// A TOML scalar from either a parsed document entry or `howdy set` text.
		struct ScalarView {
			HowdyConfigValueKind kind     = HOWDY_CONFIG_VALUE_KIND_BOOLEAN;
			bool                 boolean  = false;
			std::int64_t         integer  = 0;
			double               floating = 0.0;
			std::string_view     string;
		};

		// The one mapping from TOML scalar types to schema types: float options also accept
		// integers, integers must fit `int`, and floats must be finite and representable as a
		// normal `float` (or zero). String safety is enforced by Rust before a scalar reaches C++.
		auto ToOptionValue(const config_schema::Option &option, const ScalarView &scalar)
		    -> std::expected<config_schema::ConfigValue, std::string_view> {
			switch (option.type) {
				case config_schema::ValueType::kBoolean:
					if (scalar.kind != HOWDY_CONFIG_VALUE_KIND_BOOLEAN) {
						return std::unexpected("must be a boolean");
					}
					return config_schema::ConfigValue{scalar.boolean};
				case config_schema::ValueType::kInteger:
					if (scalar.kind != HOWDY_CONFIG_VALUE_KIND_INTEGER) {
						return std::unexpected("must be an integer");
					}
					if (scalar.integer < std::numeric_limits<int>::min() ||
					    scalar.integer > std::numeric_limits<int>::max()) {
						return std::unexpected("is out of range");
					}
					return config_schema::ConfigValue{static_cast<int>(scalar.integer)};
				case config_schema::ValueType::kFloatingPoint: {
					double number = 0.0;
					if (scalar.kind == HOWDY_CONFIG_VALUE_KIND_FLOAT) {
						number = scalar.floating;
					} else if (scalar.kind == HOWDY_CONFIG_VALUE_KIND_INTEGER) {
						number = static_cast<double>(scalar.integer);
					} else {
						return std::unexpected("must be a number");
					}
					if (!std::isfinite(number) ||
					    std::fabs(number) >
					        static_cast<double>(std::numeric_limits<float>::max())) {
						return std::unexpected("must be finite");
					}
					const auto narrowed = static_cast<float>(number);
					// A non-zero value that float cannot represent normally would silently become
					// zero or lose precision; reject it instead.
					if (number != 0.0 && !std::isnormal(narrowed)) {
						return std::unexpected("is too close to zero");
					}
					return config_schema::ConfigValue{narrowed};
				}
				case config_schema::ValueType::kString:
					if (scalar.kind != HOWDY_CONFIG_VALUE_KIND_STRING) {
						return std::unexpected("must be a string");
					}
					return config_schema::ConfigValue{std::string(scalar.string)};
			}
			return std::unexpected("has an unsupported type");
		}

		auto TypedValue(const config_schema::Option &option, const HowdyConfigEntry &entry,
		                std::string *error) -> std::optional<config_schema::ConfigValue> {
			auto typed = ToOptionValue(option, {
			                                       .kind     = entry.kind,
			                                       .boolean  = entry.boolean,
			                                       .integer  = entry.integer,
			                                       .floating = entry.floating,
			                                       .string   = Text(entry.string),
			                                   });
			if (!typed.has_value()) {
				*error = PositionPrefix(entry.line, entry.column) + OptionName(option) + " " +
				         std::string(typed.error());
				return std::nullopt;
			}
			return std::move(*typed);
		}

		auto EditFor(const config_schema::Option &option, const config_schema::ConfigValue &value)
		    -> HowdyConfigEdit {
			HowdyConfigEdit edit{
			    .table    = View(option.section),
			    .key      = View(option.key),
			    .kind     = HOWDY_CONFIG_VALUE_KIND_BOOLEAN,
			    .boolean  = 0,
			    .integer  = 0,
			    .floating = 0.0F,
			    .string   = View({}),
			};
			std::visit(
			    [&edit](const auto &typed) -> void {
				    using Value = std::decay_t<decltype(typed)>;
				    if constexpr (std::is_same_v<Value, bool>) {
					    edit.boolean = typed ? 1 : 0;
				    } else if constexpr (std::is_same_v<Value, int>) {
					    edit.kind    = HOWDY_CONFIG_VALUE_KIND_INTEGER;
					    edit.integer = typed;
				    } else if constexpr (std::is_same_v<Value, float>) {
					    edit.kind     = HOWDY_CONFIG_VALUE_KIND_FLOAT;
					    edit.floating = typed;
				    } else {
					    edit.kind   = HOWDY_CONFIG_VALUE_KIND_STRING;
					    edit.string = View(typed);
				    }
			    },
			    value);
			return edit;
		}

		auto EditFailure(std::string error) -> ConfigEditResult {
			return {.ok = false, .content = {}, .error = std::move(error)};
		}

		auto EditOutcome(HowdyFfiStatus status, const EditResultHandle &handle)
		    -> ConfigEditResult {
			if (status != HOWDY_FFI_STATUS_OK || handle == nullptr) {
				return EditFailure(std::string(FfiFailureMessage(status)));
			}
			bool             has_error = false;
			HowdyConfigError error{};
			if (howdy_config_edit_result_error(handle.get(), &has_error, &error) !=
			    HOWDY_FFI_STATUS_OK) {
				return EditFailure(
				    std::string(FfiFailureMessage(HOWDY_FFI_STATUS_INVALID_ARGUMENT)));
			}
			if (has_error) {
				return EditFailure(ErrorMessage(error));
			}
			HowdyStr updated{};
			if (howdy_config_edit_result_content(handle.get(), &updated) != HOWDY_FFI_STATUS_OK) {
				return EditFailure(
				    std::string(FfiFailureMessage(HOWDY_FFI_STATUS_INVALID_ARGUMENT)));
			}
			return {.ok = true, .content = std::string(Text(updated)), .error = {}};
		}

	}  // namespace

	auto ConfigDocument::FromContent(std::string_view content) -> ConfigDocument {
		ConfigDocument document;
		if (content.size() > kMaxConfigFileSize) {
			document.error_ = "config exceeds maximum size";
			return document;
		}

		HowdyConfigDocument *raw    = nullptr;
		const auto           status = howdy_config_parse(View(content), &raw);
		const DocumentHandle handle(raw);
		if (status != HOWDY_FFI_STATUS_OK || handle == nullptr) {
			document.error_ = FfiFailureMessage(status);
			return document;
		}

		bool             has_error = false;
		HowdyConfigError error{};
		if (howdy_config_document_error(handle.get(), &has_error, &error) != HOWDY_FFI_STATUS_OK) {
			document.error_ = FfiFailureMessage(HOWDY_FFI_STATUS_INVALID_ARGUMENT);
			return document;
		}
		if (has_error) {
			document.error_ = ErrorMessage(error);
			return document;
		}

		std::size_t table_count = 0;
		std::size_t entry_count = 0;
		if (howdy_config_document_counts(handle.get(), &table_count, &entry_count) !=
		    HOWDY_FFI_STATUS_OK) {
			document.error_ = FfiFailureMessage(HOWDY_FFI_STATUS_INVALID_ARGUMENT);
			return document;
		}

		std::vector<std::string_view> table_names;
		table_names.reserve(table_count);
		for (std::size_t index = 0; index < table_count; ++index) {
			HowdyConfigTable table{};
			if (howdy_config_document_table(handle.get(), index, &table) != HOWDY_FFI_STATUS_OK) {
				document.error_ = FfiFailureMessage(HOWDY_FFI_STATUS_INVALID_ARGUMENT);
				return document;
			}
			const auto name = Text(table.name);
			if (!config_schema::IsRuntimeConfigSection(name)) {
				document.error_ = PositionPrefix(table.line, table.column) + "unknown table [" +
				                  std::string(name) + "]";
				return document;
			}
			table_names.push_back(name);
		}

		for (std::size_t index = 0; index < entry_count; ++index) {
			HowdyConfigEntry entry{};
			if (howdy_config_document_entry(handle.get(), index, &entry) != HOWDY_FFI_STATUS_OK ||
			    entry.table_index >= table_names.size()) {
				document.error_ = FfiFailureMessage(HOWDY_FFI_STATUS_INVALID_ARGUMENT);
				document.values_.clear();
				return document;
			}
			const auto  section = table_names.at(entry.table_index);
			const auto  key     = Text(entry.key);
			const auto *option  = config_schema::RuntimeConfigOption(section, key);
			if (option == nullptr) {
				document.error_ = PositionPrefix(entry.line, entry.column) + "unknown key \"" +
				                  std::string(section) + "." + std::string(key) + "\"";
				document.values_.clear();
				return document;
			}
			auto typed = TypedValue(*option, entry, &document.error_);
			if (!typed.has_value()) {
				// Never report success for a partly read document.
				document.values_.clear();
				return document;
			}
			// An empty string selects the default, like an omitted key.
			if (const auto *text = std::get_if<std::string>(&*typed);
			    text == nullptr || !text->empty()) {
				document.values_.emplace(option->id, std::move(*typed));
			}
		}
		// Success is explicit: no failure path can be mistaken for success, even one whose
		// message happens to be empty.
		document.ok_ = true;
		return document;
	}

	auto ConfigDocument::Ok() const -> bool {
		return ok_;
	}

	auto ConfigDocument::Error() const -> const std::string & {
		return error_;
	}

	auto ConfigDocument::Value(std::string_view section, std::string_view name) const
	    -> const config_schema::ConfigValue * {
		const auto *option = config_schema::RuntimeConfigOption(section, name);
		if (option == nullptr) {
			return nullptr;
		}
		const auto found = values_.find(option->id);
		return found == values_.end() ? nullptr : &found->second;
	}

	namespace {

		template <typename Type>
		auto TypedOr(const config_schema::ConfigValue *value, const Type &fallback) -> Type {
			const auto *typed = value == nullptr ? nullptr : std::get_if<Type>(value);
			return typed == nullptr ? fallback : *typed;
		}

	}  // namespace

	auto ConfigDocument::GetBool(std::string_view section, std::string_view name,
	                             bool fallback) const -> bool {
		return TypedOr(Value(section, name), fallback);
	}

	auto ConfigDocument::GetInt(std::string_view section, std::string_view name, int fallback) const
	    -> int {
		return TypedOr(Value(section, name), fallback);
	}

	auto ConfigDocument::GetFloat(std::string_view section, std::string_view name,
	                              float fallback) const -> float {
		return TypedOr(Value(section, name), fallback);
	}

	auto ConfigDocument::GetString(std::string_view section, std::string_view name,
	                               const std::string &fallback) const -> std::string {
		return TypedOr(Value(section, name), fallback);
	}

	auto SetConfigValue(std::string_view content, const config_schema::Option &option,
	                    const config_schema::ConfigValue &value) -> ConfigEditResult {
		if (content.size() > kMaxConfigFileSize) {
			return EditFailure("config exceeds maximum size");
		}
		const auto             edit   = EditFor(option, value);
		HowdyConfigEditResult *raw    = nullptr;
		const auto             status = howdy_config_set(View(content), &edit, &raw);
		return EditOutcome(status, EditResultHandle(raw));
	}

	auto IsSafeConfigText(std::string_view text) -> bool {
		if (text.size() > kMaxConfigFileSize) {
			return false;
		}
		bool safe = false;
		return howdy_config_is_safe_text(View(text), &safe) == HOWDY_FFI_STATUS_OK && safe;
	}

	auto ParseOptionValueText(const config_schema::Option &option, std::string_view text)
	    -> std::optional<config_schema::ConfigValue> {
		if (text.size() > kMaxConfigFileSize) {
			return std::nullopt;
		}
		if (option.type == config_schema::ValueType::kString) {
			if (!IsSafeConfigText(text)) {
				return std::nullopt;
			}
			return config_schema::ConfigValue{std::string(text)};
		}
		bool              valid = false;
		HowdyConfigScalar scalar{};
		if (howdy_config_parse_scalar(View(text), &valid, &scalar) != HOWDY_FFI_STATUS_OK ||
		    !valid) {
			return std::nullopt;
		}
		auto typed = ToOptionValue(option, {
		                                       .kind     = scalar.kind,
		                                       .boolean  = scalar.boolean,
		                                       .integer  = scalar.integer,
		                                       .floating = scalar.floating,
		                                       .string   = {},
		                                   });
		if (!typed.has_value()) {
			return std::nullopt;
		}
		return std::move(*typed);
	}

}  // namespace howdy::native
