#include "storage/user_model_label.hpp"
#include "test_support.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace {

	using howdy::test::Expect;

	auto ExpectTruncateUtf8ToMaxBytes() -> bool {
		struct Case {
			const char      *label;
			std::string_view input;
			std::size_t      max_bytes;
			std::string_view expected;
		};

		const auto cases = std::to_array<Case>({
		    {
		        .label     = "empty string with zero limit",
		        .input     = "",
		        .max_bytes = 0,
		        .expected  = "",
		    },
		    {
		        .label     = "empty string with positive limit",
		        .input     = "",
		        .max_bytes = 5,
		        .expected  = "",
		    },
		    {
		        .label     = "ASCII with zero limit",
		        .input     = "abc",
		        .max_bytes = 0,
		        .expected  = "",
		    },
		    {
		        .label     = "ASCII exact fit",
		        .input     = "abc",
		        .max_bytes = 3,
		        .expected  = "abc",
		    },
		    {
		        .label     = "ASCII shorter than limit",
		        .input     = "abc",
		        .max_bytes = 10,
		        .expected  = "abc",
		    },
		    {
		        .label     = "ASCII truncated mid-string",
		        .input     = "abcdef",
		        .max_bytes = 3,
		        .expected  = "abc",
		    },
		    {
		        .label     = "2-byte UTF-8 split drops trailing code point",
		        .input     = "a\xC3\xA9",
		        .max_bytes = 2,
		        .expected  = "a",
		    },
		    {
		        .label     = "2-byte UTF-8 exact boundary keeps code point",
		        .input     = "a\xC3\xA9",
		        .max_bytes = 3,
		        .expected  = "a\xC3\xA9",
		    },
		    {
		        .label     = "3-byte UTF-8 split after lead byte",
		        .input     = "a\xE0\xB8\x81",
		        .max_bytes = 2,
		        .expected  = "a",
		    },
		    {
		        .label     = "3-byte UTF-8 split before final byte",
		        .input     = "a\xE0\xB8\x81",
		        .max_bytes = 3,
		        .expected  = "a",
		    },
		    {
		        .label     = "3-byte UTF-8 exact boundary keeps code point",
		        .input     = "a\xE0\xB8\x81",
		        .max_bytes = 4,
		        .expected  = "a\xE0\xB8\x81",
		    },
		    {
		        .label     = "4-byte UTF-8 split at byte 1 yields empty",
		        .input     = "\xF0\x9F\x93\xB7x",
		        .max_bytes = 1,
		        .expected  = "",
		    },
		    {
		        .label     = "4-byte UTF-8 split at byte 3 yields empty",
		        .input     = "\xF0\x9F\x93\xB7x",
		        .max_bytes = 3,
		        .expected  = "",
		    },
		    {
		        .label     = "4-byte UTF-8 exact boundary keeps emoji",
		        .input     = "\xF0\x9F\x93\xB7x",
		        .max_bytes = 4,
		        .expected  = "\xF0\x9F\x93\xB7",
		    },
		});

		bool ok = true;
		for (const auto &test_case : cases) {
			const auto actual =
			    howdy::native::TruncateUtf8ToMaxBytes(test_case.input, test_case.max_bytes);
			ok &= Expect(actual == test_case.expected && actual.size() <= test_case.max_bytes &&
			                 howdy::native::IsValidUtf8(actual),
			             std::string("TruncateUtf8ToMaxBytes: ") + test_case.label);
		}
		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	ok &=
	    Expect(howdy::native::IsValidModelLabel("Laptop camera"), "normal model label is accepted");
	ok &= Expect(howdy::native::IsValidModelLabel(""), "empty model label remains accepted");
	ok &= Expect(!howdy::native::IsValidModelLabel("bad/name"),
	             "model label containing slash is rejected");
	ok &= Expect(!howdy::native::IsValidModelLabel("bad\\name"),
	             "model label containing backslash is rejected");
	ok &= Expect(!howdy::native::IsValidModelLabel("bad\nname"),
	             "model label containing newline is rejected");
	ok &= Expect(!howdy::native::IsValidModelLabel("bad\tname"),
	             "model label containing tab is rejected");

	ok &= Expect(howdy::native::IsValidModelLabel("\xE0\xB8\x81\xE0\xB8\xA5\xE0\xB9\x89\xE0\xB8\xAD"
	                                              "\xE0\xB8\x87 \xF0\x9F\x93\xB7"),
	             "multibyte UTF-8 label is accepted");
	ok &= Expect(!howdy::native::IsValidModelLabel("ab\xE0\xB8"),
	             "label with truncated UTF-8 sequence is rejected");
	ok &= Expect(!howdy::native::IsValidModelLabel("\xFF"), "label with invalid byte is rejected");
	ok &= Expect(!howdy::native::IsValidModelLabel("\xC0\x80"),
	             "label with overlong encoding is rejected");
	ok &= Expect(!howdy::native::IsValidModelLabel("\xED\xA0\x80"),
	             "label with surrogate code point is rejected");
	ok &= Expect(!howdy::native::IsValidModelLabel("\xF4\x90\x80\x80"),
	             "label above U+10FFFF is rejected");
	ok &= Expect(!howdy::native::IsValidModelLabel("\xE0\xB8\x41"),
	             "label with bad continuation byte is rejected");
	ok &= ExpectTruncateUtf8ToMaxBytes();

	return ok ? 0 : 1;
}
