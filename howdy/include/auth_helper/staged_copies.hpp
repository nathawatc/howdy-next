#pragma once

#include <cstdint>
#include <ostream>
#include <string>
#include <utility>

namespace howdy::native::auth_helper {

	enum class StagedPurgeTrigger : std::uint8_t {
		kModelsChanged,  // the command just changed, or may have changed, the stored models
		kRetry,          // nothing changed now; finish a purge an earlier run could not
	};

	// Removes the copies of a user's face models that howdy-auth-helper staged for unprivileged
	// PAM consumers, so removed models do not stay readable until the next verification.
	// Best effort: anything it could not remove is reported on `output`, and it never fails the
	// caller. Does nothing unless run as root. A retry stays silent about a user name that does
	// not resolve, since nothing was changed for it.
	void PurgeStagedCopiesForUser(const std::string &user, StagedPurgeTrigger trigger,
	                              std::ostream &output) noexcept;

	// Dependency seam for commands that purge after changing a user's models.
	using PurgeStagedCopiesFn = void (*)(void *context, const std::string &user,
	                                     StagedPurgeTrigger trigger);

	// An optional PurgeStagedCopiesFn bound to one user, for commands that purge at several
	// points. A missing seam is never called.
	class StagedCopyPurger {
	public:
		StagedCopyPurger(PurgeStagedCopiesFn purge, void *context, std::string user)
		    : purge_(purge)
		    , context_(context)
		    , user_(std::move(user)) {}

		void operator()(StagedPurgeTrigger trigger) const {
			if (purge_ != nullptr) {
				purge_(context_, user_, trigger);
			}
		}

	private:
		PurgeStagedCopiesFn purge_   = nullptr;
		void               *context_ = nullptr;
		std::string         user_;
	};

	// Production PurgeStagedCopiesFn: ignores `context` and reports on standard output.
	void PurgeStagedCopiesToStdout(void *context, const std::string &user,
	                               StagedPurgeTrigger trigger) noexcept;

}  // namespace howdy::native::auth_helper
