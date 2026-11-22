#pragma once

#include "app/command_invocation.hpp"
#include "storage/user_model_store.hpp"
#include "storage/user_model_types.hpp"

#include <string>

namespace howdy::native::list_internal {

	using ListUserModelEntriesFn = howdy::native::UserModelListResult (*)(void *context,
	                                                                      const std::string &user);

	struct ListDependencies {
		void                  *context                 = nullptr;
		ListUserModelEntriesFn list_user_model_entries = nullptr;
	};

	// Binds every dependency to `store`, which must outlive the returned dependencies.
	[[nodiscard]] auto StoreDependencies(howdy::native::UserModelStore &store) -> ListDependencies;

	auto ListMainWithDependencies(const CommandInvocation &invocation,
	                              const ListDependencies  &dependencies) -> int;

}  // namespace howdy::native::list_internal
