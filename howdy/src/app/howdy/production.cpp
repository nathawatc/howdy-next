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

#include <array>
#include <unistd.h>

namespace {

	using howdy::native::howdy_internal::CommandMain;

	auto ResolveInvokingIdentityDependency(void *context) -> howdy::native::InvokingIdentityResult {
		(void)context;
		return howdy::native::ResolveInvokingIdentity();
	}

	auto EffectiveUid(void *context) -> uid_t {
		(void)context;
		return geteuid();
	}

	auto ProductionCommandMains()
	    -> std::array<CommandMain, static_cast<std::size_t>(howdy::native::CommandId::kCount)> {
		std::array<CommandMain, static_cast<std::size_t>(howdy::native::CommandId::kCount)>
		    command_mains{};
		command_mains[static_cast<std::size_t>(howdy::native::CommandId::kAdd)]     = AddMain;
		command_mains[static_cast<std::size_t>(howdy::native::CommandId::kClear)]   = ClearMain;
		command_mains[static_cast<std::size_t>(howdy::native::CommandId::kConfig)]  = ConfigMain;
		command_mains[static_cast<std::size_t>(howdy::native::CommandId::kDisable)] = DisableMain;
		command_mains[static_cast<std::size_t>(howdy::native::CommandId::kDownloadModels)] =
		    DownloadModelsMain;
		command_mains[static_cast<std::size_t>(howdy::native::CommandId::kList)]     = ListMain;
		command_mains[static_cast<std::size_t>(howdy::native::CommandId::kRemove)]   = RemoveMain;
		command_mains[static_cast<std::size_t>(howdy::native::CommandId::kSet)]      = SetMain;
		command_mains[static_cast<std::size_t>(howdy::native::CommandId::kSnapshot)] = SnapshotMain;
		command_mains[static_cast<std::size_t>(howdy::native::CommandId::kTest)]     = TestMain;
		return command_mains;
	}

}  // namespace

auto HowdyMain(int argc, char **argv) -> int {
	return howdy::native::howdy_internal::HowdyMainWithDependencies(
	    argc, argv,
	    {
	        .resolve_invoking_identity = ResolveInvokingIdentityDependency,
	        .effective_uid             = EffectiveUid,
	        .command_mains             = ProductionCommandMains(),
	    });
}
