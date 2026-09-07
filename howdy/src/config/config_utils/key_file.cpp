#include "config/config_utils.hpp"
#include "internal.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace howdy::native {

	auto IsSafeIniScalarValue(std::string_view value) -> bool {
		if (!value.empty() && value.front() == '[') {
			return false;
		}

		return std::ranges::all_of(value, [](const char ch) -> bool {
			return ch != '\0' && ch != '\n' && ch != '\r';
		});
	}

}  // namespace howdy::native

namespace howdy::native::config_utils_internal {

	namespace {

		auto IniIdentifierEqual(std::string_view left, std::string_view right) -> bool {
			return left.size() == right.size() &&
			       std::ranges::equal(
			           left, right, [](unsigned char left_char, unsigned char right_char) -> bool {
				           return std::tolower(left_char) == std::tolower(right_char);
			           });
		}

		auto SectionName(std::string_view line) -> std::optional<std::string_view> {
			constexpr std::string_view utf8_bom = "\xEF\xBB\xBF";
			if (line.starts_with(utf8_bom)) {
				line.remove_prefix(utf8_bom.size());
			}
			if (line.empty() || line.front() != '[') {
				return std::nullopt;
			}
			const auto end = line.find(']');
			if (end == std::string_view::npos) {
				return std::nullopt;
			}
			return line.substr(1, end - 1);
		}

		auto AssignmentName(std::string_view line) -> std::optional<std::string_view> {
			const auto separator = line.find_first_of("=:");
			if (separator == std::string_view::npos) {
				return std::nullopt;
			}
			auto name = line.substr(0, separator);
			while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
				name.remove_suffix(1);
			}
			return name;
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
	    -> ConfigLineReplaceResult {
		std::string_view current_section;
		bool             has_previous_name = false;
		std::string     *matching_line     = nullptr;
		for (auto &line : lines) {
			const auto stripped_pos = line.find_first_not_of(" \t");
			if (stripped_pos == std::string::npos) {
				continue;
			}
			const std::string_view stripped(line.data() + stripped_pos, line.size() - stripped_pos);
			if (stripped.front() == ';' || stripped.front() == '#') {
				continue;
			}
			if (has_previous_name && stripped_pos != 0) {
				continue;
			}
			if (const auto section = SectionName(stripped)) {
				current_section   = *section;
				has_previous_name = false;
				continue;
			}

			const auto name = AssignmentName(stripped);
			if (name.has_value()) {
				has_previous_name = true;
				if (!IniIdentifierEqual(current_section, replacement.section) ||
				    !IniIdentifierEqual(*name, replacement.key)) {
					continue;
				}
			} else {
				const auto name_end = stripped.find_first_of(" \t");
				if (!IniIdentifierEqual(current_section, replacement.section) ||
				    name_end == std::string_view::npos ||
				    !IniIdentifierEqual(stripped.substr(0, name_end), replacement.key)) {
					continue;
				}
				has_previous_name = true;
			}

			if (matching_line != nullptr) {
				return ConfigLineReplaceResult::kDuplicate;
			}
			matching_line = &line;
		}
		if (matching_line == nullptr) {
			return ConfigLineReplaceResult::kNotFound;
		}
		*matching_line = replacement.key;
		*matching_line += " = ";
		*matching_line += replacement.value;
		*matching_line += '\n';
		return ConfigLineReplaceResult::kReplaced;
	}

}  // namespace howdy::native::config_utils_internal
