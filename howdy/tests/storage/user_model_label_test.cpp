#include "storage/user_model_label.hpp"
#include "test_support.hpp"

namespace {

	using howdy::test::Expect;

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

	return ok ? 0 : 1;
}
