#include "auth_helper/staged_copies.hpp"

#include "auth_helper/runtime.hpp"
#include "support/passwd_lookup.hpp"

#include <iostream>
#include <ostream>
#include <string>

#include <unistd.h>

namespace howdy::native::auth_helper {

	namespace {

		void PurgeOrReport(const std::string &user, StagedPurgeTrigger trigger,
		                   std::ostream &output) {
			// Only root can change the runtime root, and the staged copies are keyed by UID.
			if (geteuid() != 0) {
				return;
			}
			const auto account = LookupPasswdByName(user);
			if (!account.has_value()) {
				if (trigger == StagedPurgeTrigger::kRetry) {
					return;
				}
				output << "Could not resolve " << user << " to a UID; any staged copy of its face "
				       << "models under " << RuntimeRoot().string() << " stays until reboot.\n";
				return;
			}

			// A retry changed nothing, so it does not wait for a staging run to finish.
			const auto result =
			    PurgeRuntimeAuthFiles({.uid = account->uid}, trigger == StagedPurgeTrigger::kRetry
				                                                 ? StagedPurgeLockWait::kNone
				                                                 : StagedPurgeLockWait::kBrief);
			if (result == StagedPurgeResult::kPurged ||
			    result == StagedPurgeResult::kNothingStaged) {
				return;
			}
			const auto root = RuntimeRoot().string();
			// A retry changed no models, so a kept copy may hold only current ones; it is reported
			// without claiming that a removed model is still readable.
			if (trigger == StagedPurgeTrigger::kRetry) {
				if (result == StagedPurgeResult::kInUse) {
					// The busy lock may be the allocation lock, so no copy need exist at all.
					output << "The staged copies of the face models for " << user << " under "
					       << root << " are busy and were not checked.\n";
				} else {
					output << "Could not check the staged copies of the face models for " << user
					       << " under " << root << ".\n";
				}
				return;
			}
			// A kept slot is only rewritten if a later prepare happens to pick it, so neither
			// message promises that it goes away on its own.
			if (result == StagedPurgeResult::kInUse) {
				output
				    << "A staged copy of the face models for " << user << " under " << root
				    << " is in use by an authentication or a staging run and was kept; it may stay "
				    << "readable by " << user << " until reboot. Running the same command again "
				    << "once the authentication has ended removes it, although that run reports "
				    << "the models as already gone.\n";
			} else {
				output << "Could not remove the staged copy of the face models for " << user
				       << " under " << root << "; it may stay readable by " << user
				       << " until reboot.\n";
			}
		}

	}  // namespace

	void PurgeStagedCopiesForUser(const std::string &user, StagedPurgeTrigger trigger,
	                              std::ostream &output) noexcept {
		// Runs after the command's models already changed, so a failure here must not turn a
		// committed change into a failed command.
		try {
			PurgeOrReport(user, trigger, output);
		} catch (...) {
			try {
				output << "Could not check the staged copies of the face models for " << user
				       << ".\n";
			} catch (...) {  // NOLINT(bugprone-empty-catch)
			                 // Nothing is left to report with.
			}
		}
	}

	void PurgeStagedCopiesToStdout([[maybe_unused]] void *context, const std::string &user,
	                               StagedPurgeTrigger trigger) noexcept {
		PurgeStagedCopiesForUser(user, trigger, std::cout);
	}

}  // namespace howdy::native::auth_helper
