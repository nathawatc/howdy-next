#pragma once

#include "storage/user_models.hpp"

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

	auto clear_main_with_dependencies(int argc, char **argv, const ClearDependencies &dependencies)
	    -> int;

}  // namespace howdy::native::clear_internal
