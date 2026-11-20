#pragma once

#include "app/command_invocation.hpp"
#include "storage/user_model_types.hpp"
#include "support/file_security/validation_root.hpp"

#include <string>

namespace howdy::native::clear_internal {

	using InspectUserModelFileFn =
	    howdy::native::UserModelInspectResult (*)(void *context, const std::string &user);

	using ClearUserModelEntriesIfUnchangedFn = howdy::native::UserModelMutationResult (*)(
	    void *context, const std::string &user,
	    const howdy::native::UserModelFileSnapshot &expected_snapshot);

	struct ClearDependencies {
		void                              *context                               = nullptr;
		InspectUserModelFileFn             inspect_user_model_file               = nullptr;
		ClearUserModelEntriesIfUnchangedFn clear_user_model_entries_if_unchanged = nullptr;
	};

	auto ClearMainWithDependencies(const CommandInvocation &invocation,
	                               const ClearDependencies &dependencies) -> int;

	auto ClearMainWithValidationRoot(const CommandInvocation               &invocation,
	                                 file_security_internal::ValidationRoot validation_root) -> int;

}  // namespace howdy::native::clear_internal
