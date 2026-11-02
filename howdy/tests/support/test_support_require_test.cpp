#include "test_support.hpp"

#include <iostream>
#include <sstream>

namespace {

	using howdy::test::Expect;
	using howdy::test::IsRequired;
	using howdy::test::ScopedEnvironmentVariable;
	using howdy::test::ScopedStreamBuffer;
	using howdy::test::SkipUnavailable;

	auto TestRequiredListParsing() -> bool {
		bool ok = true;
		{
			const ScopedEnvironmentVariable unset("HOWDY_TEST_REQUIRE");
			ok &= Expect(!IsRequired("acl"), "unset list requires nothing");
		}
		{
			const ScopedEnvironmentVariable empty("HOWDY_TEST_REQUIRE", "");
			ok &= Expect(!IsRequired("acl"), "empty list requires nothing");
			ok &= Expect(!IsRequired(""), "empty capability is never required");
		}
		{
			const ScopedEnvironmentVariable list("HOWDY_TEST_REQUIRE", "locale-th,acl,seccomp");
			ok &= Expect(IsRequired("locale-th"), "first entry is required");
			ok &= Expect(IsRequired("acl"), "middle entry is required");
			ok &= Expect(IsRequired("seccomp"), "last entry is required");
			ok &= Expect(!IsRequired("locale"), "prefix of an entry is not required");
			ok &= Expect(!IsRequired("locale-th,acl"), "separator is not part of a capability");
			ok &= Expect(!IsRequired("locale-nl"), "unlisted capability is not required");
		}
		return ok;
	}

	auto TestSkipUnavailable() -> bool {
		bool                     ok = true;
		const std::ostringstream errors;
		{
			const ScopedStreamBuffer redirect(std::cerr, errors.rdbuf());
			{
				const ScopedEnvironmentVariable unset("HOWDY_TEST_REQUIRE");
				ok &= Expect(SkipUnavailable("acl", "no acl"), "optional capability skips");
			}
			{
				const ScopedEnvironmentVariable list("HOWDY_TEST_REQUIRE", "acl");
				ok &= Expect(!SkipUnavailable("acl", "no acl"),
				             "required capability fails instead of skipping");
				ok &= Expect(SkipUnavailable("seccomp", "no seccomp"),
				             "unrelated capability still skips");
			}
		}
		const auto output = errors.str();
		ok &= Expect(output.contains("SKIP: no acl\n"), "optional skip is reported as SKIP");
		ok &= Expect(output.contains("FAIL: required capability 'acl' unavailable: no acl\n"),
		             "required capability is reported as FAIL");
		ok &= Expect(output.contains("SKIP: no seccomp\n"), "unrelated skip is reported as SKIP");
		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= TestRequiredListParsing();
	ok &= TestSkipUnavailable();
	return ok ? 0 : 1;
}
