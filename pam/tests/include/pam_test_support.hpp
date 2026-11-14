#pragma once

#include <security/pam_appl.h>

// Unused here. Includers that name pid_t or ssize_t rely on <sys/types.h> declaring them first:
// glibc declares each typedef in whichever header comes first, and misc-include-cleaner attributes
// it there. Including <sys/types.h> in those tests is not enough, because their standard headers
// come earlier and can declare pid_t via <sched.h>.
#include <sys/types.h>  // IWYU pragma: keep

struct pam_handle {};

namespace howdy::test {

	inline auto OpaquePamHandleForTest() noexcept -> pam_handle_t * {
		static pam_handle handle{};
		return &handle;
	}

	inline auto PamResponseSentinelForTest() noexcept -> pam_response * {
		static thread_local pam_response sentinel{};
		return &sentinel;
	}

	inline auto PamStartForTest(const char *service, const char *username,
	                            const struct pam_conv *conversation, pam_handle_t **handle) -> int {
#ifdef HOWDY_PAM_TEST_CONFDIR
		// pam/tests/confdir holds only an empty "other" service, which Linux-PAM uses for every
		// service name without its own file, so these tests never read /etc/pam.d.
		return pam_start_confdir(service, username, conversation, HOWDY_PAM_TEST_CONFDIR, handle);
#else
		// Without pam_start_confdir, service names resolve against the host's /etc/pam.d.
		return pam_start(service, username, conversation, handle);
#endif
	}

}  // namespace howdy::test
