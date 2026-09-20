#include "app/command_catalog.hpp"
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
#include "test_support.hpp"

#include <cstddef>

namespace {

	using howdy::native::CommandCatalog;
	using howdy::native::CommandId;
	using howdy::native::CommandKind;
	using howdy::native::howdy_internal::CommandMains;
	using howdy::test::Expect;

	auto ExpectedCommandMains() -> CommandMains {
		CommandMains expected{};
		expected.at(static_cast<std::size_t>(CommandId::kAdd))            = AddMain;
		expected.at(static_cast<std::size_t>(CommandId::kClear))          = ClearMain;
		expected.at(static_cast<std::size_t>(CommandId::kConfig))         = ConfigMain;
		expected.at(static_cast<std::size_t>(CommandId::kDisable))        = DisableMain;
		expected.at(static_cast<std::size_t>(CommandId::kDownloadModels)) = DownloadModelsMain;
		expected.at(static_cast<std::size_t>(CommandId::kList))           = ListMain;
		expected.at(static_cast<std::size_t>(CommandId::kRemove))         = RemoveMain;
		expected.at(static_cast<std::size_t>(CommandId::kSet))            = SetMain;
		expected.at(static_cast<std::size_t>(CommandId::kSnapshot))       = SnapshotMain;
		expected.at(static_cast<std::size_t>(CommandId::kTest))           = TestMain;
		return expected;
	}

}  // namespace

auto main() -> int {
	const auto actual   = howdy::native::howdy_internal::ProductionCommandMains();
	const auto expected = ExpectedCommandMains();
	bool       ok       = true;
	for (const auto &command : CommandCatalog()) {
		const auto command_main = actual.at(static_cast<std::size_t>(command.id));
		if (command.kind == CommandKind::kEntrypoint) {
			ok &= Expect(command_main != nullptr,
			             "every production entrypoint command has a concrete binding");
		} else {
			ok &= Expect(command_main == nullptr,
			             "non-entrypoint commands do not have production entrypoint bindings");
		}
	}
	for (std::size_t index = 0; index < expected.size(); ++index) {
		ok &= Expect(actual.at(index) == expected.at(index),
		             "production command ID binds the expected public entrypoint");
	}
	return ok ? 0 : 1;
}
