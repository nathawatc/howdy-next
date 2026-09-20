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

	auto ResolveInvokingIdentityDependency(void *context) -> howdy::native::InvokingIdentityResult {
		(void)context;
		return howdy::native::ResolveInvokingIdentity();
	}

	auto EffectiveUid(void *context) -> uid_t {
		(void)context;
		return geteuid();
	}

}  // namespace

auto howdy::native::howdy_internal::ProductionCommandMains() -> CommandMains {
	CommandMains command_mains{};
	command_mains.at(static_cast<std::size_t>(CommandId::kAdd))            = AddMain;
	command_mains.at(static_cast<std::size_t>(CommandId::kClear))          = ClearMain;
	command_mains.at(static_cast<std::size_t>(CommandId::kConfig))         = ConfigMain;
	command_mains.at(static_cast<std::size_t>(CommandId::kDisable))        = DisableMain;
	command_mains.at(static_cast<std::size_t>(CommandId::kDownloadModels)) = DownloadModelsMain;
	command_mains.at(static_cast<std::size_t>(CommandId::kList))           = ListMain;
	command_mains.at(static_cast<std::size_t>(CommandId::kRemove))         = RemoveMain;
	command_mains.at(static_cast<std::size_t>(CommandId::kSet))            = SetMain;
	command_mains.at(static_cast<std::size_t>(CommandId::kSnapshot))       = SnapshotMain;
	command_mains.at(static_cast<std::size_t>(CommandId::kTest))           = TestMain;
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
