#include "app/command_catalog.hpp"
#include "app/howdy.hpp"
#include "app/howdy/internal.hpp"
#include "cli/add.hpp"
#include "cli/clear.hpp"
#include "cli/config.hpp"
#include "cli/disable.hpp"
#include "cli/download_models.hpp"
#include "cli/list.hpp"
#include "cli/remove.hpp"
#include "cli/set.hpp"
#include "cli/snapshot.hpp"
#include "cli/test.hpp"
#include "support/invoking_user.hpp"

#include <cstddef>

#include <unistd.h>

#include <sys/types.h>

namespace {

	auto ResolveInvokingIdentityDependency([[maybe_unused]] void *context)
	    -> howdy::native::InvokingIdentityResult {
		return howdy::native::ResolveInvokingIdentity();
	}

	auto EffectiveUid([[maybe_unused]] void *context) -> uid_t {
		return geteuid();
	}

	constexpr auto MainForCommand(howdy::native::CommandId id) noexcept
	    -> howdy::native::howdy_internal::CommandMain {
		switch (id) {
			case howdy::native::CommandId::kAdd:
				return AddMain;
			case howdy::native::CommandId::kClear:
				return ClearMain;
			case howdy::native::CommandId::kConfig:
				return ConfigMain;
			case howdy::native::CommandId::kDisable:
				return DisableMain;
			case howdy::native::CommandId::kDownloadModels:
				return DownloadModelsMain;
			case howdy::native::CommandId::kList:
				return ListMain;
			case howdy::native::CommandId::kRemove:
				return RemoveMain;
			case howdy::native::CommandId::kSet:
				return SetMain;
			case howdy::native::CommandId::kSnapshot:
				return SnapshotMain;
			case howdy::native::CommandId::kTest:
				return TestMain;
			case howdy::native::CommandId::kVersion:
			case howdy::native::CommandId::kCount:
				return nullptr;
		}
		return nullptr;
	}

}  // namespace

auto howdy::native::howdy_internal::ProductionCommandMains() -> CommandMains {
	CommandMains command_mains{};
	for (std::size_t index = 0; index < command_mains.size(); ++index) {
		command_mains.at(index) = MainForCommand(static_cast<CommandId>(index));
	}
	return command_mains;
}

auto HowdyMain(int argc, char **argv) -> int {
	return howdy::native::howdy_internal::HowdyMainWithDependencies(
	    argc, argv,
	    {
	        .resolve_invoking_identity = ResolveInvokingIdentityDependency,
	        .effective_uid             = EffectiveUid,
	        .command_mains             = howdy::native::howdy_internal::ProductionCommandMains(),
	    });
}
