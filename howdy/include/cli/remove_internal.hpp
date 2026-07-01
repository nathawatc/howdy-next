#pragma once

#include "storage/user_models.hpp"

#include <string>

namespace howdy::native::remove_internal {

	using ListUserModelEntriesFn = howdy::native::UserModelListResult (*)(void *context,
	                                                                      const std::string &user);

	using RemoveUserModelEntryIfMatchesFn = howdy::native::UserModelMutationResult (*)(
	    void *context, const std::string &user,
	    const howdy::native::UserModelEntryExpectation &expected);

	struct RemoveDependencies {
		void                           *context                            = nullptr;
		ListUserModelEntriesFn          list_user_model_entries            = nullptr;
		RemoveUserModelEntryIfMatchesFn remove_user_model_entry_if_matches = nullptr;
	};

	auto remove_main_with_dependencies(int argc, char **argv,
	                                   const RemoveDependencies &dependencies) -> int;

}  // namespace howdy::native::remove_internal
