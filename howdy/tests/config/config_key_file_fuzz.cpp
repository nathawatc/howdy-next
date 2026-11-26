#include "config/config_key_file_fuzz.hpp"

#include "../../src/config/config_reader/internal.hpp"
#include "../../src/config/config_utils/internal.hpp"
#include "config/config_schema.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// Differential fuzz of the INI editor against inih, through the reader production uses.
//
// Input layout: one byte selecting the option to set, one byte giving the length of its new value
// (modulo kMaxValueBytes), the value, then the config. Config bytes below 0x80 stand for
// themselves; higher bytes select a token, so that random input often holds the option and the
// shapes around it.
namespace howdy::test {

	namespace {

		namespace config_utils = native::config_utils_internal;
		namespace schema       = native::config_schema;

		constexpr std::size_t kMaxValueBytes = 24;
		// inih silently cuts a longer line (INI_MAX_LINE, 200 by default) and flags a parse error,
		// so such a config never passes the validation an update goes through.
		constexpr std::size_t kMaxLineBytes = 150;

		// "%s" and "%k" stand for the target's section and key.
		constexpr auto kTokens = std::to_array<std::string_view>({
		    "\xEF\xBB\xBF",
		    "[%s]\n",
		    "\n[%s]\n",
		    "[%s",
		    "[%s] ; [x]\n",
		    "%k",
		    "\n%k = ",
		    "\n%k",
		    "\n%k: old",
		    "\n  %k = ",
		    "\n%k = old\n  second",
		    "\n%k = old ; note\n",
		    "\n%kx = ",
		    "\n[core]\n",
		    "\n[video]\n",
		    "\n[other]\n",
		    "\n[other",
		    "\ndisabled = ",
		    "\nabort_if_ssh true",
		    "\nplain = ",
		    "\n= x\n",
		    "\n  continued",
		    "\n  timeout = 3",
		    "\n  [video]",
		    "\n  /dev/v4l/by-path/pci-0000:00:14.0-usb-0:1:1.0-video-index0",
		    "\nmalformed\n",
		    "\n",
		    "\n\n",
		    "\r\n",
		    "\n  ",
		    "\n\t",
		    "\n\f",
		    "\n\v ",
		    "\n; note\n",
		    "\n  # note\n",
		    " ; ",
		    ";",
		    "#",
		    " = ",
		    "=",
		    ":",
		    "[",
		    "]",
		    "value",
		});

		// Every value inih reads, under "section=key" with the case folded, as INIReader stores it.
		using IniValues = std::map<std::string, std::string>;

		auto IniName(std::string_view section, std::string_view key) -> std::string {
			return std::format("{}={}", section, key);
		}

		auto ReadValues(const native::ConfigReader &reader) -> IniValues {
			IniValues values;
			for (const auto &section : reader.Sections()) {
				for (const auto &key : reader.Keys(section)) {
					values.emplace(IniName(section, key), reader.Get(section, key, ""));
				}
			}
			return values;
		}

		// The escape that std::format has as "{:?}" only since GCC 15.
		auto Escaped(std::string_view text) -> std::string {
			std::string escaped = "\"";
			for (const char ch : text) {
				const auto byte = static_cast<unsigned char>(ch);
				if (ch == '\n') {
					escaped += "\\n";
				} else if (byte < 0x20 || byte >= 0x7f || ch == '"' || ch == '\\') {
					escaped += std::format("\\x{{{:02x}}}", byte);
				} else {
					escaped += ch;
				}
			}
			return escaped + '"';
		}

		[[noreturn]] void Fail(std::string_view reason, const KeyFileFuzzTarget &target,
		                       std::string_view before, std::string_view after) {
			std::cerr << reason << "\n[" << target.section << "] " << target.key << " -> "
			          << Escaped(target.value) << "\nbefore: " << Escaped(before)
			          << "\nafter:  " << Escaped(after) << "\n";
			std::abort();
		}

		void AppendToken(std::string &content, std::string_view token,
		                 const KeyFileFuzzTarget &target) {
			while (!token.empty()) {
				const auto plain = token.substr(0, token.find('%'));
				content += plain;
				token.remove_prefix(plain.size());
				if (!token.empty()) {
					content += token.at(1) == 's' ? target.section : target.key;
					token.remove_prefix(2);
				}
			}
		}

		auto HasOverlongLine(std::string_view content) -> bool {
			std::size_t line_bytes = 0;
			return std::ranges::any_of(content, [&line_bytes](const char ch) -> bool {
				line_bytes = ch == '\n' ? 0 : line_bytes + 1;
				return line_bytes > kMaxLineBytes;
			});
		}

	}  // namespace

	auto CheckKeyFileReplacement(const KeyFileFuzzTarget &target, std::string_view content)
	    -> config_utils::ConfigLineReplaceResult {
		const std::string before(content);
		// UpdateConfigValue refuses a damaged config before it edits, and so must every reader.
		if (native::FindConfigDamage(before) != native::ConfigDamage::kNone) {
			if (native::ConfigReader(before).Ok()) {
				Fail("a damaged config was accepted", target, before, before);
			}
			return config_utils::ConfigLineReplaceResult::kNotFound;
		}
		auto       lines   = config_utils::SplitLinesPreserveNewlines(before);
		const auto outcome = config_utils::ReplaceLineValue(
		    lines, {.section = target.section, .key = target.key, .value = target.value});
		const auto after = config_utils::JoinLines(lines);
		if (outcome.result != config_utils::ConfigLineReplaceResult::kReplaced) {
			if (after != before) {
				Fail("a refused update changed the config", target, before, after);
			}
			return outcome.result;
		}

		const native::ConfigReader before_reader(before);
		const native::ConfigReader after_reader(after);
		if (before_reader.Ok() && !after_reader.Ok()) {
			Fail("the update broke a config that parsed", target, before, after);
		}
		auto       before_values = ReadValues(before_reader);
		auto       after_values  = ReadValues(after_reader);
		const auto target_name   = IniName(target.section, target.key);
		const auto updated       = after_values.find(target_name);
		if (updated == after_values.end() || updated->second != target.value) {
			Fail("inih does not read the new value", target, before, after);
		}
		before_values.erase(target_name);
		after_values.erase(target_name);
		if (before_values != after_values) {
			Fail("the update changed another option", target, before, after);
		}
		return outcome.result;
	}

	auto RunKeyFileFuzzInput(std::span<const std::uint8_t> input)
	    -> std::optional<config_utils::ConfigLineReplaceResult> {
		if (input.size() < 2) {
			return std::nullopt;
		}
		const auto  options = schema::RuntimeConfigOptions();
		const auto &option  = options.subspan(input.front() % options.size()).front();
		const auto  rest    = input.subspan(2);
		const auto  value_size =
		    std::min<std::size_t>(input.subspan(1).front() % kMaxValueBytes, rest.size());

		std::string value;
		for (const auto byte : rest.first(value_size)) {
			value.push_back(static_cast<char>(byte));
		}
		if (!config_utils::IsSafeIniScalarValue(value) ||
		    !config_utils::ReadsBackUnchanged(value)) {
			return std::nullopt;
		}

		const KeyFileFuzzTarget target{
		    .section = option.section,
		    .key     = option.key,
		    .value   = value,
		};
		std::string content;
		for (const auto byte : rest.subspan(value_size)) {
			if (byte >= 0x80) {
				AppendToken(content, kTokens.at((byte - 0x80U) % kTokens.size()), target);
			} else {
				content.push_back(static_cast<char>(byte));
			}
		}
		if (HasOverlongLine(content)) {
			return std::nullopt;
		}
		return CheckKeyFileReplacement(target, content);
	}

}  // namespace howdy::test

// NOLINTBEGIN(readability-identifier-naming)
extern "C" auto LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) -> int;

extern "C" auto LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) -> int {
	(void)howdy::test::RunKeyFileFuzzInput(std::span(data, size));
	return 0;
}

// NOLINTEND(readability-identifier-naming)
