#pragma once

#include "app/command_invocation.hpp"
#include "storage/user_model_store.hpp"
#include "storage/user_model_types.hpp"

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

	// Binds every dependency to `store`, which must outlive the returned dependencies.
	[[nodiscard]] auto StoreDependencies(howdy::native::UserModelStore &store) -> ClearDependencies;

	auto ClearMainWithDependencies(const CommandInvocation &invocation,
	                               const ClearDependencies &dependencies) -> int;

}  // namespace howdy::native::clear_internal
