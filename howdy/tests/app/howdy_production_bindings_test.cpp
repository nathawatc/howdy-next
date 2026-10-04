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

#include <array>
#include <cstddef>
#include <iostream>
#include <sstream>
#include <string>

#include <sys/types.h>

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

	auto RootForDispatch([[maybe_unused]] void *context) -> uid_t {
		return 0;
	}

	auto ProductionSetDispatch() -> bool {
		std::array<std::string, 4> arguments{"howdy", "set", "disabled", "line\nbreak"};
		std::array<char *, 4>      argv{arguments.at(0).data(), arguments.at(1).data(),
		                                arguments.at(2).data(), arguments.at(3).data()};
		std::ostringstream const   output;
		std::ostringstream const   error;
		auto                      *old_output = std::cout.rdbuf(output.rdbuf());
		auto                      *old_error  = std::cerr.rdbuf(error.rdbuf());
		const int status = howdy::native::howdy_internal::HowdyMainWithDependencies(
		    static_cast<int>(argv.size()), argv.data(),
		    {.effective_uid = RootForDispatch,
			 .command_mains = howdy::native::howdy_internal::ProductionCommandMains()});
		std::cout.rdbuf(old_output);
		std::cerr.rdbuf(old_error);
		return Expect(
		    status == 1 &&
		        output.str() ==
		            "Config values must be single-line scalars and cannot start with [\n" &&
		        error.str().empty(),
		    "production dispatcher invokes SetMain and rejects unsafe value");
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
	ok &= ProductionSetDispatch();
	return ok ? 0 : 1;
}
