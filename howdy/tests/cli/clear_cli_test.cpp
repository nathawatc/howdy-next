#include "app/command_invocation.hpp"
#include "cli/clear/internal.hpp"
#include "storage/user_model_status.hpp"
#include "storage/user_model_types.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include <stdlib.h>

namespace {

	using howdy::test::Expect;
	using howdy::test::StreamRedirect;
	using howdy::test::WriteFile;

	struct ClearCliTestContext {
		howdy::native::UserModelInspectResult  inspect_result;
		howdy::native::UserModelMutationResult clear_result;
		int                                    inspect_calls = 0;
		int                                    clear_calls   = 0;
		std::string                            inspected_user;
		std::string                            cleared_user;
		howdy::native::UserModelFileSnapshot   received_snapshot;
	};

	auto ValidSnapshot() -> howdy::native::UserModelFileSnapshot {
		return {
		    .dev            = 11,
		    .inode          = 22,
		    .size           = 33,
		    .mtime_seconds  = 44,
		    .mtime_nanosecs = 55,
		    .ctime_seconds  = 66,
		    .ctime_nanosecs = 77,
		};
	}

	auto SuccessContext() -> ClearCliTestContext {
		ClearCliTestContext context;
		context.inspect_result = {
		    .status   = howdy::native::UserModelStatus::kOk,
		    .snapshot = ValidSnapshot(),
		};
		context.clear_result = {.status = howdy::native::UserModelStatus::kOk};
		return context;
	}

	auto InspectCallback(void *raw_context, const std::string &user)
	    -> howdy::native::UserModelInspectResult {
		auto *context = static_cast<ClearCliTestContext *>(raw_context);
		++context->inspect_calls;
		context->inspected_user = user;
		return context->inspect_result;
	}

	auto ClearCallback(void *raw_context, const std::string &user,
	                   const howdy::native::UserModelFileSnapshot &expected_snapshot)
	    -> howdy::native::UserModelMutationResult {
		auto *context = static_cast<ClearCliTestContext *>(raw_context);
		++context->clear_calls;
		context->cleared_user      = user;
		context->received_snapshot = expected_snapshot;
		return context->clear_result;
	}

	auto
	RunClearWithDependencies(const howdy::native::CommandInvocation                 &invocation,
	                         const howdy::native::clear_internal::ClearDependencies &dependencies,
	                         const std::string &input = {}) -> std::pair<int, std::string> {
		std::istringstream const input_stream(input);
		std::ostringstream const output_stream;
		StreamRedirect const     redirect(std::cin, input_stream.rdbuf(), std::cout,
		                                  output_stream.rdbuf());
		const int                result =
		    howdy::native::clear_internal::ClearMainWithDependencies(invocation, dependencies);
		return {result, output_stream.str()};
	}

	auto RunClear(ClearCliTestContext &context, const howdy::native::CommandInvocation &invocation,
	              const std::string &input = {}) -> std::pair<int, std::string> {
		return RunClearWithDependencies(invocation,
		                                {
		                                    .context                 = &context,
		                                    .inspect_user_model_file = InspectCallback,
		                                    .clear_user_model_entries_if_unchanged = ClearCallback,
		                                },
		                                input);
	}

	auto IncompleteDependenciesAbortWithoutCallbacks() -> bool {
		auto context                                    = SuccessContext();
		auto [null_inspect_result, null_inspect_output] = RunClearWithDependencies(
		    {.resolved_user = "alice"}, {
		                                    .context                               = &context,
		                                    .inspect_user_model_file               = nullptr,
		                                    .clear_user_model_entries_if_unchanged = ClearCallback,
		                                });
		bool ok = true;
		ok &= Expect(null_inspect_result == 1, "null inspect callback returns 1");
		ok &= Expect(null_inspect_output.empty(), "null inspect callback stays silent");
		ok &= Expect(context.inspect_calls == 0 && context.clear_calls == 0,
		             "null inspect callback skips callbacks");

		auto [null_clear_result, null_clear_output] = RunClearWithDependencies(
		    {.resolved_user = "alice"}, {
		                                    .context                 = &context,
		                                    .inspect_user_model_file = InspectCallback,
		                                    .clear_user_model_entries_if_unchanged = nullptr,
		                                });
		ok &= Expect(null_clear_result == 1, "null clear callback returns 1");
		ok &= Expect(null_clear_output.empty(), "null clear callback stays silent");
		ok &= Expect(context.inspect_calls == 0 && context.clear_calls == 0,
		             "null clear callback skips callbacks");
		return ok;
	}

	auto MissingResolvedUserAbortsWithoutCallbacks() -> bool {
		auto context          = SuccessContext();
		auto [result, output] = RunClear(context, {});
		bool ok               = true;
		ok &= Expect(result == 1, "missing resolved user returns 1");
		ok &= Expect(output.empty(), "missing resolved user stays silent");
		ok &= Expect(context.inspect_calls == 0 && context.clear_calls == 0,
		             "missing resolved user skips callbacks");
		return ok;
	}

	auto InspectionOutcomesPreserveMessages() -> bool {
		bool ok = true;
		for (const auto &[status, error, snapshot, expected, expected_exit] : std::vector<
		         std::tuple<howdy::native::UserModelStatus, std::string,
				            std::optional<howdy::native::UserModelFileSnapshot>, std::string, int>>{
		         {
		             howdy::native::UserModelStatus::kNoModelDirectory,
		             {},
		             std::nullopt,
		             "No face models found.\n",
		             0,
		         },
		         {
		             howdy::native::UserModelStatus::kNoModel,
		             {},
		             std::nullopt,
		             "No face models found.\n",
		             0,
		         },
		         {
		             howdy::native::UserModelStatus::kInsecurePath,
		             "storage failed",
		             std::nullopt,
		             "storage failed\n",
		             1,
		         },
		         {
		             howdy::native::UserModelStatus::kOk,
		             {},
		             std::nullopt,
		             "Failed to inspect user model file\n",
		             1,
		         },
		     }) {
			auto context           = SuccessContext();
			context.inspect_result = {
			    .status        = status,
			    .error_message = error,
			    .snapshot      = snapshot,
			};
			auto [result, output] =
			    RunClear(context, {.resolved_user = "alice", .assume_yes = true});
			ok &= Expect(result == expected_exit, "inspection outcome returns expected status");
			ok &= Expect(output == expected, "inspection outcome preserves message");
			ok &= Expect(context.inspect_calls == 1 && context.clear_calls == 0,
			             "inspection outcome inspects once and skips clear");
		}
		return ok;
	}

	auto RejectedConfirmationAborts() -> bool {
		auto context          = SuccessContext();
		auto [result, output] = RunClear(context, {.resolved_user = "alice"}, "n\n");
		return Expect(result == 1, "rejected confirmation returns 1") &&
		       Expect(output == "This will remove all face models for alice\n"
		                        "Continue? [y/N]: "
		                        "\nNo confirmation received; aborting.\n",
		              "rejected confirmation preserves output") &&
		       Expect(context.inspect_calls == 1 && context.clear_calls == 0,
		              "rejected confirmation inspects once and skips clear");
	}

	auto AcceptedConfirmationPassesSnapshot() -> bool {
		auto context          = SuccessContext();
		auto [result, output] = RunClear(context, {.resolved_user = "alice"}, "Y\n");
		const auto &snapshot  = context.received_snapshot;
		bool        ok        = true;
		ok &= Expect(result == 0, "accepted confirmation returns 0");
		ok &= Expect(context.inspect_calls == 1 && context.inspected_user == "alice",
		             "accepted confirmation inspects requested user once");
		ok &= Expect(context.clear_calls == 1 && context.cleared_user == "alice",
		             "accepted confirmation clears requested user once");
		ok &= Expect(snapshot.dev == 11 && snapshot.inode == 22 && snapshot.size == 33 &&
		                 snapshot.mtime_seconds == 44 && snapshot.mtime_nanosecs == 55 &&
		                 snapshot.ctime_seconds == 66 && snapshot.ctime_nanosecs == 77,
		             "accepted confirmation passes complete snapshot unchanged");
		ok &= Expect(output == "This will remove all face models for alice\n"
		                       "Continue? [y/N]: \nModels cleared\n",
		             "accepted confirmation preserves output");
		return ok;
	}

	auto YesFlagBypassesConfirmation() -> bool {
		auto context          = SuccessContext();
		auto [result, output] = RunClear(context, {.resolved_user = "alice", .assume_yes = true});
		return Expect(result == 0, "-y returns 0") &&
		       Expect(output == "\nModels cleared\n", "-y preserves output without prompt") &&
		       Expect(context.inspect_calls == 1 && context.clear_calls == 1,
		              "-y inspects and clears once");
	}

	auto ClearOutcomesPreserveMessages() -> bool {
		bool ok = true;
		for (const auto &[status, error, expected, expected_exit] :
		     std::vector<std::tuple<howdy::native::UserModelStatus, std::string, std::string, int>>{
		         {
		             howdy::native::UserModelStatus::kNoModelDirectory,
		             {},
		             "No face models found.\n",
		             0,
		         },
		         {
		             howdy::native::UserModelStatus::kNoModel,
		             {},
		             "No face models found.\n",
		             0,
		         },
		         {
		             howdy::native::UserModelStatus::kModelChanged,
		             "model changed",
		             "model changed\n",
		             1,
		         },
		     }) {
			auto context         = SuccessContext();
			context.clear_result = {.status = status, .error_message = error};
			auto [result, output] =
			    RunClear(context, {.resolved_user = "alice", .assume_yes = true});
			ok &= Expect(result == expected_exit, "clear outcome returns expected status");
			ok &= Expect(output == expected, "clear outcome preserves message");
			ok &= Expect(context.inspect_calls == 1 && context.clear_calls == 1,
			             "clear outcome invokes callbacks once");
		}
		return ok;
	}

	auto SuccessfulClearPreservesOutput() -> bool {
		auto context          = SuccessContext();
		auto [result, output] = RunClear(context, {.resolved_user = "alice", .assume_yes = true});
		return Expect(result == 0, "successful clear returns 0") &&
		       Expect(output == "\nModels cleared\n", "successful clear preserves output");
	}

	auto BoundaryAwareClearRemovesUnparseableModelFiles() -> bool {
		namespace fs = std::filesystem;

		std::string temp_template = (fs::current_path() / "howdy-clear-cli-test-XXXXXX").string();
		const char *temp_path     = mkdtemp(temp_template.data());
		if (!Expect(temp_path != nullptr, "create unique integration temp dir")) {
			return false;
		}

		bool                                  ok = true;
		const howdy::test::TemporaryDirectory temp_dir{fs::path(temp_path)};
		const auto                           &temp_root  = temp_dir.Path();
		const auto                            models_dir = temp_root / "models";
		const auto                            model_path = models_dir / "alice.dat";
		std::error_code                       ec;
		fs::create_directories(models_dir, ec);
		ok &= Expect(!ec, "create integration models dir");
		const howdy::test::ScopedEnvironmentVariable models_env("HOWDY_USER_MODELS_DIR",
		                                                        models_dir.string());

		auto const run_boundary_aware_clear = [&] -> int {
			std::istringstream const input_stream;
			std::ostringstream const output_stream;
			StreamRedirect const     redirect(std::cin, input_stream.rdbuf(), std::cout,
			                                  output_stream.rdbuf());
			return howdy::native::clear_internal::ClearMainWithValidationRoot(
			    {.resolved_user = "alice", .assume_yes = true}, {temp_root});
		};

		ok &= Expect(WriteFile(model_path, "not-json"), "write malformed model JSON");
		ok &=
		    Expect(run_boundary_aware_clear() == 0, "boundary-aware clear removes malformed JSON");
		ok &= Expect(!fs::exists(model_path), "malformed JSON model deleted");

		std::string oversized_json =
		    R"json([{"id":1,"label":"large","data":[[0.1]],"padding":")json";
		oversized_json.append((1024 * 1024) + 1, 'x');
		oversized_json += "\"}]";
		ok &= Expect(WriteFile(model_path, oversized_json), "write oversized model JSON");
		ok &=
		    Expect(run_boundary_aware_clear() == 0, "boundary-aware clear removes oversized JSON");
		ok &= Expect(!fs::exists(model_path), "oversized JSON model deleted");

		ok &= Expect(WriteFile(model_path, R"({"id":1})"), "write wrong-shape model JSON");
		ok &= Expect(run_boundary_aware_clear() == 0,
		             "boundary-aware clear removes wrong-shape JSON");
		ok &= Expect(!fs::exists(model_path), "wrong-shape JSON model deleted");
		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= IncompleteDependenciesAbortWithoutCallbacks();
	ok &= MissingResolvedUserAbortsWithoutCallbacks();
	ok &= InspectionOutcomesPreserveMessages();
	ok &= RejectedConfirmationAborts();
	ok &= AcceptedConfirmationPassesSnapshot();
	ok &= YesFlagBypassesConfirmation();
	ok &= ClearOutcomesPreserveMessages();
	ok &= SuccessfulClearPreservesOutput();
	ok &= BoundaryAwareClearRemovesUnparseableModelFiles();
	return ok ? 0 : 1;
}
