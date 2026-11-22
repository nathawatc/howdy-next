#include "cli/list.hpp"

#include "app/command_invocation.hpp"
#include "cli/list/internal.hpp"
#include "storage/user_model_status.hpp"
#include "storage/user_model_store.hpp"
#include "storage/user_model_types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

	constexpr int kListExitOk    = 0;
	constexpr int kListExitAbort = 1;

	auto CsvField(const std::string_view value) -> std::string {
		if (!value.contains(',') && !value.contains('"')) {
			return std::string(value);
		}
		std::string escaped;
		escaped.reserve(value.size() + 2);
		escaped.push_back('"');
		for (const char character : value) {
			if (character == '"') {
				escaped.push_back('"');
			}
			escaped.push_back(character);
		}
		escaped.push_back('"');
		return escaped;
	}

	auto FormatModelTimestamp(std::int64_t time_value) -> std::string {
		std::array<char, 32> buffer{};
		std::tm              local_time{};
		if (std::in_range<std::time_t>(time_value)) {
			const auto timestamp = static_cast<std::time_t>(time_value);
			if (::localtime_r(&timestamp, &local_time) != nullptr &&
			    std::strftime(buffer.data(), buffer.size(), "%Y-%m-%d %H:%M:%S", &local_time) !=
			        0) {
				return buffer.data();
			}
		}
		return "invalid-time";
	}

	void PrintModelEntry(const howdy::native::UserModelEntry &model, bool plain) {
		std::cout << model.id;
		if (plain) {
			std::cout << ",";
		} else {
			constexpr std::size_t id_column_width = 4;
			const auto            id_size         = std::to_string(model.id).size();
			std::cout << std::string(id_size < id_column_width ? id_column_width - id_size : 0,
			                         ' ');
		}
		std::cout << FormatModelTimestamp(model.time) << (plain ? "," : "  ")
		          << (plain ? CsvField(model.label) : model.label) << "\n";
	}

	auto ListUserModelEntriesFromStore(void *context, const std::string &user)
	    -> howdy::native::UserModelListResult {
		const auto *store = static_cast<const howdy::native::UserModelStore *>(context);
		if (store == nullptr) {
			return {
			    .status        = howdy::native::UserModelStatus::kParseError,
			    .error_message = "User model store is not initialized",
			};
		}
		return store->ListEntries(user, {});
	}

}  // namespace

auto howdy::native::list_internal::StoreDependencies(howdy::native::UserModelStore &store)
    -> ListDependencies {
	return {.context = &store, .list_user_model_entries = ListUserModelEntriesFromStore};
}

auto howdy::native::list_internal::ListMainWithDependencies(
    const howdy::native::CommandInvocation &invocation, const ListDependencies &dependencies)
    -> int {
	if (dependencies.list_user_model_entries == nullptr) {
		return kListExitAbort;
	}

	if (!invocation.resolved_user.has_value()) {
		return kListExitAbort;
	}
	const auto &user   = *invocation.resolved_user;
	const auto  models = dependencies.list_user_model_entries(dependencies.context, user);
	if (howdy::native::Classify(models.status) == howdy::native::UserModelOutcome::kAbsent) {
		if (!invocation.plain) {
			std::cout
			    << "No face models found. Use the add command to add a face model for this user.\n";
		}
		return kListExitOk;
	}
	if (models.status != howdy::native::UserModelStatus::kOk) {
		if (!invocation.plain) {
			std::cout << models.error_message << "\n";
		}
		return kListExitAbort;
	}
	for (const auto &model : models.entries) {
		PrintModelEntry(model, invocation.plain);
	}

	std::cout << "\n";
	return kListExitOk;
}

auto ListMain(const howdy::native::CommandInvocation &invocation) -> int {
	auto store = howdy::native::UserModelStore::Production();
	return howdy::native::list_internal::ListMainWithDependencies(
	    invocation, howdy::native::list_internal::StoreDependencies(store));
}
