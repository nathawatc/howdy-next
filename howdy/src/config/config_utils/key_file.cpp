#include "config/config_schema.hpp"
#include "internal.hpp"
#include "support/ascii.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace howdy::native::config_utils_internal {

	auto IsSafeIniScalarValue(std::string_view value) -> bool {
		if (!value.empty() && value.front() == '[') {
			return false;
		}

		return std::ranges::all_of(value, [](const char ch) -> bool {
			return ch != '\0' && ch != '\n' && ch != '\r';
		});
	}

	namespace {

		// Drops what inih reads as an inline comment: a ';' after whitespace.
		auto WithoutInlineComment(std::string_view line) -> std::string_view {
			for (std::size_t index = 1; index < line.size(); ++index) {
				if (line.at(index) == ';' && IsAsciiSpace(line.at(index - 1))) {
					return line.substr(0, index);
				}
			}
			return line;
		}

		auto SectionName(std::string_view line) -> std::optional<std::string_view> {
			if (line.empty() || line.front() != '[') {
				return std::nullopt;
			}
			line           = WithoutInlineComment(line);
			const auto end = line.find(']');
			if (end == std::string_view::npos) {
				return std::nullopt;
			}
			return line.substr(1, end - 1);
		}

		auto FirstWord(std::string_view line) -> std::string_view {
			return line.substr(0, line.find_first_of(" \t\r\n\v\f"));
		}

		auto AssignmentName(std::string_view line) -> std::optional<std::string_view> {
			line                 = WithoutInlineComment(line);
			const auto separator = line.find_first_of("=:");
			if (separator == std::string_view::npos) {
				return std::nullopt;
			}
			auto name = line.substr(0, separator);
			while (!name.empty() && IsAsciiSpace(name.back())) {
				name.remove_suffix(1);
			}
			return name;
		}

		auto IsKnownOption(std::string_view name) -> bool {
			return std::ranges::any_of(config_schema::RuntimeConfigOptions(),
			                           [name](const auto &option) -> bool {
				                           return AsciiEqualIgnoreCase(option.key, name);
			                           });
		}

		// Whether a continuation line reads as a setting or section of its own. An absolute path
		// such as /dev/v4l/by-path/pci-0000:00:14.0-... holds separators too but is a plain value.
		auto LooksLikeSetting(std::string_view stripped) -> bool {
			if (stripped.starts_with('[')) {
				return true;
			}
			if (AssignmentName(stripped).has_value() && !stripped.starts_with('/')) {
				return true;
			}
			return IsKnownOption(FirstWord(stripped));
		}

		// Mirrors inih's line ownership. Its previous name, which indented lines continue, is set
		// by an assignment, cleared by a section, and kept across blank, comment and malformed
		// lines.
		struct IniScanState {
			std::string_view current_section;
			bool             has_previous_name  = false;
			bool             target_is_previous = false;
			// A matched line without a separator is malformed to inih, so it owns nothing until the
			// rewrite turns it into an assignment.
			bool                       bare_target_open = false;
			std::optional<std::size_t> match_start;
			// Only the continuation lines are erased so interleaved blank/comment lines
			// survive the rewrite.
			std::vector<std::size_t>   continuation_lines;
			std::optional<std::size_t> ambiguous_line;
		};

		void MarkAmbiguous(IniScanState &state, std::size_t index) {
			if (!state.ambiguous_line.has_value()) {
				state.ambiguous_line = index;
			}
		}

		void ProcessIndentedLine(IniScanState &state, std::size_t index,
		                         std::string_view stripped) {
			// The rewrite would hand this line, so far another option's, to the target.
			if (state.bare_target_open) {
				MarkAmbiguous(state, index);
			}
			if (!state.has_previous_name || !state.target_is_previous) {
				return;
			}
			// inih reads an indented `key = value` or `[section]` as part of the old value, but
			// erasing one would silently drop what its author meant as a setting.
			if (LooksLikeSetting(stripped)) {
				MarkAmbiguous(state, index);
			}
			state.continuation_lines.push_back(index);
		}

		void StartOwner(IniScanState &state, bool has_name) {
			state.has_previous_name  = has_name;
			state.target_is_previous = false;
			state.bare_target_open   = false;
		}

		// Returns false when a duplicate key is detected.
		auto ProcessIniLine(IniScanState &state, std::size_t index, std::string_view line_view,
		                    const ConfigLineReplacement &replacement) -> bool {
			constexpr std::string_view utf8_bom = "\xEF\xBB\xBF";
			if (index == 0 && line_view.starts_with(utf8_bom)) {
				line_view.remove_prefix(utf8_bom.size());
			}
			const auto stripped_pos = static_cast<std::size_t>(
			    std::ranges::find_if_not(line_view, IsAsciiSpace) - line_view.begin());
			if (stripped_pos == line_view.size()) {
				return true;
			}
			const auto stripped = line_view.substr(stripped_pos);
			if (stripped.front() == ';' || stripped.front() == '#') {
				return true;
			}
			if (stripped_pos != 0) {
				ProcessIndentedLine(state, index, stripped);
				if (state.has_previous_name) {
					return true;
				}
			}
			if (stripped.front() == '[') {
				if (const auto section = SectionName(stripped)) {
					state.current_section = *section;
					StartOwner(state, false);
				}
				return true;
			}
			const auto name = AssignmentName(stripped);
			if (name.has_value()) {
				StartOwner(state, !name->empty());
			}
			if (!AsciiEqualIgnoreCase(state.current_section, replacement.section) ||
			    !AsciiEqualIgnoreCase(name.value_or(FirstWord(stripped)), replacement.key)) {
				return true;
			}
			if (state.match_start.has_value()) {
				return false;
			}
			state.match_start        = index;
			state.target_is_previous = name.has_value();
			state.bare_target_open   = !name.has_value();
			return true;
		}

	}  // namespace

	auto SplitLinesPreserveNewlines(const std::string &content) -> std::vector<std::string> {
		std::vector<std::string> lines;
		std::size_t              start = 0;
		while (start < content.size()) {
			const auto end = content.find('\n', start);
			if (end == std::string::npos) {
				lines.push_back(content.substr(start));
				break;
			}
			lines.push_back(content.substr(start, (end - start) + 1));
			start = end + 1;
		}
		return lines;
	}

	auto JoinLines(const std::vector<std::string> &lines) -> std::string {
		std::string content;
		for (const auto &line : lines) {
			content += line;
		}
		return content;
	}

	auto ReplaceLineValue(std::vector<std::string> &lines, ConfigLineReplacement replacement)
	    -> ConfigLineReplaceOutcome {
		IniScanState state;
		for (std::size_t index = 0; index < lines.size(); ++index) {
			if (!ProcessIniLine(state, index, lines.at(index), replacement)) {
				return {.result = ConfigLineReplaceResult::kDuplicate};
			}
		}
		if (!state.match_start.has_value()) {
			return {.result = ConfigLineReplaceResult::kNotFound};
		}
		if (state.ambiguous_line.has_value()) {
			return {
			    .result = ConfigLineReplaceResult::kAmbiguousContinuation,
			    .line   = *state.ambiguous_line + 1,
			};
		}
		auto &first_line = lines.at(*state.match_start);
		first_line       = replacement.key;
		first_line += " = ";
		first_line += replacement.value;
		first_line += '\n';
		for (const auto index : std::views::reverse(state.continuation_lines)) {
			lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(index));
		}
		return {.result = ConfigLineReplaceResult::kReplaced};
	}

}  // namespace howdy::native::config_utils_internal
