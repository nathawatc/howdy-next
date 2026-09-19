#include "support/passwd_lookup.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <span>
#include <string_view>

#include <pwd.h>

namespace {

	using howdy::test::Expect;

	auto LargeRecordLookup(passwd *entry, char *buffer, std::size_t buffer_size, passwd **result)
	    -> int {
		constexpr std::size_t required_buffer_size = std::size_t{128} * 1024;
		if (buffer_size < required_buffer_size) {
			*result = nullptr;
			return ERANGE;
		}

		constexpr std::string_view name  = "large-record-user";
		constexpr std::string_view home  = "/home/large-record-user";
		constexpr std::string_view shell = "/bin/sh";
		std::span<char>            storage(buffer, buffer_size);

		auto copy_field = [&storage](std::string_view field) -> char * {
			char *start = storage.data();
			std::ranges::copy(field, storage.begin());
			storage.subspan(field.size(), 1).front() = '\0';
			storage                                  = storage.subspan(field.size() + 1);
			return start;
		};

		entry->pw_uid   = 1234;
		entry->pw_gid   = 5678;
		entry->pw_name  = copy_field(name);
		entry->pw_dir   = copy_field(home);
		entry->pw_shell = copy_field(shell);
		*result         = entry;
		return 0;
	}

}  // namespace

auto main() -> int {
	const auto record = howdy::native::passwd_lookup_detail::LookupPasswd(LargeRecordLookup);
	bool       ok     = true;
	ok &= Expect(record.has_value(), "passwd lookup grows beyond the former 64 KiB limit");
	if (record.has_value()) {
		ok &= Expect(record->uid == 1234 && record->gid == 5678,
		             "passwd lookup preserves numeric identity");
		ok &= Expect(record->name == "large-record-user",
		             "passwd lookup copies the account name out of scratch storage");
		ok &= Expect(record->home == "/home/large-record-user" && record->shell == "/bin/sh",
		             "passwd lookup copies path fields out of scratch storage");
	}
	return ok ? 0 : 1;
}
