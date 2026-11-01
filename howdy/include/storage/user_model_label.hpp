#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string_view>

namespace howdy::native {

	namespace user_model_label_detail {

		struct Utf8Lead {
			std::size_t   length     = 0;
			unsigned char min_second = 0x80;
			unsigned char max_second = 0xBF;
		};

		// RFC 3629 lead byte table; second-byte bounds reject overlongs, surrogates, > U+10FFFF.
		inline auto ClassifyUtf8Lead(const unsigned char lead) -> Utf8Lead {
			if (lead < 0x80) {
				return {.length = 1};
			}
			if (lead >= 0xC2 && lead <= 0xDF) {
				return {.length = 2};
			}
			if (lead >= 0xE0 && lead <= 0xEF) {
				return {
				    .length     = 3,
				    .min_second = static_cast<unsigned char>(lead == 0xE0 ? 0xA0 : 0x80),
				    .max_second = static_cast<unsigned char>(lead == 0xED ? 0x9F : 0xBF),
				};
			}
			if (lead >= 0xF0 && lead <= 0xF4) {
				return {
				    .length     = 4,
				    .min_second = static_cast<unsigned char>(lead == 0xF0 ? 0x90 : 0x80),
				    .max_second = static_cast<unsigned char>(lead == 0xF4 ? 0x8F : 0xBF),
				};
			}
			return {};
		}

		inline auto IsUtf8Continuation(const char raw) -> bool {
			return (static_cast<unsigned char>(raw) & 0xC0U) == 0x80U;
		}

	}  // namespace user_model_label_detail

	// Strict UTF-8 check; matches what yyjson accepts on read and write.
	inline auto IsValidUtf8(std::string_view text) -> bool {
		while (!text.empty()) {
			const auto lead =
			    user_model_label_detail::ClassifyUtf8Lead(static_cast<unsigned char>(text.front()));
			if (lead.length == 0 || text.size() < lead.length) {
				return false;
			}
			const auto tail = text.substr(1, lead.length - 1);
			if (!std::ranges::all_of(tail, user_model_label_detail::IsUtf8Continuation)) {
				return false;
			}
			if (!tail.empty()) {
				const auto second = static_cast<unsigned char>(tail.front());
				if (second < lead.min_second || second > lead.max_second) {
					return false;
				}
			}
			text.remove_prefix(lead.length);
		}
		return true;
	}

	inline auto IsValidModelLabel(const std::string_view label) -> bool {
		return IsValidUtf8(label) && std::ranges::all_of(label, [](const char raw) -> bool {
			       const auto ch = static_cast<unsigned char>(raw);
			       return raw != '/' && raw != '\\' && std::iscntrl(ch) == 0;
		       });
	}

}  // namespace howdy::native
