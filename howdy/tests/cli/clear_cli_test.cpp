#include "cli/clear.hpp"
#include "cli/clear/internal.hpp"
#include "test_support.hpp"

#include <array>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

	using howdy::test::Expect;
	using howdy::test::WriteFile;

	struct StreamRedirect {
		StreamRedirect(std::istream &input_stream, std::streambuf *new_input,
		               std::ostream &output_stream, std::streambuf *new_output)
		    : input(input_stream)
		    , output(output_stream)
		    , old_input(input_stream.rdbuf(new_input))
		    , old_output(output_stream.rdbuf(new_output)) {}

		~StreamRedirect() {
			input.rdbuf(old_input);
			output.rdbuf(old_output);
		}

		std::istream   &input;
		std::ostream   &output;
		std::streambuf *old_input;
		std::streambuf *old_output;
	};

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
	RunClearWithDependencies(std::vector<std::string>                                arguments,
	                         const howdy::native::clear_internal::ClearDependencies &dependencies,
	                         const std::string &input = {}) -> std::pair<int, std::string> {
		std::vector<char *> argv;
		argv.reserve(arguments.size());
		for (auto &argument : arguments) {
			argv.push_back(argument.data());
		}

		std::istringstream input_stream(input);
		std::ostringstream output_stream;
		StreamRedirect redirect(std::cin, input_stream.rdbuf(), std::cout, output_stream.rdbuf());
		const int      result = howdy::native::clear_internal::ClearMainWithDependencies(
		    static_cast<int>(argv.size()), argv.data(), dependencies);
		return {result, output_stream.str()};
	}

	auto RunClear(ClearCliTestContext &context, std::vector<std::string> arguments,
	              const std::string &input = {}) -> std::pair<int, std::string> {
		return RunClearWithDependencies(std::move(arguments),
		                                {
		                                    .context                 = &context,
		                                    .inspect_user_model_file = InspectCallback,
		                                    .clear_user_model_entries_if_unchanged = ClearCallback,
		                                },
		                                input);
	}

	auto MissingUserReturnsWithoutCallbacks() -> bool {
		auto context          = SuccessContext();
		auto [result, output] = RunClear(context, {"howdy-clear"});
		return Expect(result == 1, "missing user returns 1") &&
		       Expect(output.empty(), "missing user stays silent") &&
		       Expect(context.inspect_calls == 0 && context.clear_calls == 0,
		              "missing user skips callbacks");
	}

	auto PublicMissingUserReturnsError() -> bool {
		auto                  command = std::to_array("howdy-clear");
		std::array<char *, 1> argv{command.data()};
		return Expect(ClearMain(1, argv.data()) == 1, "public missing user returns status 1");
	}

	auto IncompleteDependenciesAbortWithoutCallbacks() -> bool {
		auto context                                    = SuccessContext();
		auto [null_inspect_result, null_inspect_output] = RunClearWithDependencies(
		    {"howdy-clear", "alice"}, {
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
		    {"howdy-clear", "alice"}, {
		                                  .context                               = &context,
		                                  .inspect_user_model_file               = InspectCallback,
		                                  .clear_user_model_entries_if_unchanged = nullptr,
		                              });
		ok &= Expect(null_clear_result == 1, "null clear callback returns 1");
		ok &= Expect(null_clear_output.empty(), "null clear callback stays silent");
		ok &= Expect(context.inspect_calls == 0 && context.clear_calls == 0,
		             "null clear callback skips callbacks");
		return ok;
	}

	auto InspectionOutcomesPreserveMessages() -> bool {
		bool ok = true;
		for (const auto &[status, error, snapshot, expected] : std::vector<
		         std::tuple<howdy::native::UserModelStatus, std::string,
		                    std::optional<howdy::native::UserModelFileSnapshot>, std::string>>{
		         {howdy::native::UserModelStatus::kNoModelDirectory,
		          {},
		          std::nullopt,
		          "No face models found.\n"},
		         {howdy::native::UserModelStatus::kNoModel,
		          {},
		          std::nullopt,
		          "No face models found.\n"},
		         {howdy::native::UserModelStatus::kInsecurePath, "storage failed", std::nullopt,
		          "storage failed\n"},
		         {howdy::native::UserModelStatus::kOk,
		          {},
		          std::nullopt,
		          "Failed to inspect user model file\n"},
		     }) {
			auto context           = SuccessContext();
			context.inspect_result = {
			    .status = status, .error_message = error, .snapshot = snapshot};
			auto [result, output] = RunClear(context, {"howdy-clear", "alice", "-y"});
			ok &= Expect(result == 1, "inspection outcome returns 1");
			ok &= Expect(output == expected, "inspection outcome preserves message");
			ok &= Expect(context.inspect_calls == 1 && context.clear_calls == 0,
			             "inspection outcome inspects once and skips clear");
		}
		return ok;
	}

	auto RejectedConfirmationAborts() -> bool {
		auto context          = SuccessContext();
		auto [result, output] = RunClear(context, {"howdy-clear", "alice"}, "n\n");
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
		auto [result, output] = RunClear(context, {"howdy-clear", "alice"}, "Y\n");
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
		auto [result, output] = RunClear(context, {"howdy-clear", "alice", "-y"});
		return Expect(result == 0, "-y returns 0") &&
		       Expect(output == "\nModels cleared\n", "-y preserves output without prompt") &&
		       Expect(context.inspect_calls == 1 && context.clear_calls == 1,
		              "-y inspects and clears once");
	}

	auto ClearOutcomesPreserveMessages() -> bool {
		bool ok = true;
		for (const auto &[status, error, expected] :
		     std::vector<std::tuple<howdy::native::UserModelStatus, std::string, std::string>>{
		         {howdy::native::UserModelStatus::kNoModelDirectory, {}, "No face models found.\n"},
		         {howdy::native::UserModelStatus::kNoModel, {}, "No face models found.\n"},
		         {howdy::native::UserModelStatus::kModelChanged, "model changed",
		          "model changed\n"},
		     }) {
			auto context          = SuccessContext();
			context.clear_result  = {.status = status, .error_message = error};
			auto [result, output] = RunClear(context, {"howdy-clear", "alice", "-y"});
			ok &= Expect(result == 1, "clear outcome returns 1");
			ok &= Expect(output == expected, "clear outcome preserves message");
			ok &= Expect(context.inspect_calls == 1 && context.clear_calls == 1,
			             "clear outcome invokes callbacks once");
		}
		return ok;
	}

	auto SuccessfulClearPreservesOutput() -> bool {
		auto context          = SuccessContext();
		auto [result, output] = RunClear(context, {"howdy-clear", "alice", "-y"});
		return Expect(result == 0, "successful clear returns 0") &&
		       Expect(output == "\nModels cleared\n", "successful clear preserves output");
	}

	auto BoundaryAwareClearRemovesUnparseableModelFiles() -> bool {
		namespace fs = std::filesystem;

		const char                      *existing_models_dir = std::getenv("HOWDY_USER_MODELS_DIR");
		const std::optional<std::string> saved_models_dir =
		    existing_models_dir == nullptr ? std::nullopt
		                                   : std::optional<std::string>(existing_models_dir);
		const std::string temp_template_path =
		    (fs::current_path() / "howdy-clear-cli-test-XXXXXX").string();
		std::vector<char> temp_template(temp_template_path.begin(), temp_template_path.end());
		temp_template.push_back('\0');
		const char *temp_path = mkdtemp(temp_template.data());
		if (!Expect(temp_path != nullptr, "create unique integration temp dir")) {
			return false;
		}

		bool            ok         = true;
		const fs::path  temp_root  = temp_path;
		const auto      models_dir = temp_root / "models";
		const auto      model_path = models_dir / "alice.dat";
		std::error_code ec;

		fs::create_directories(models_dir, ec);
		ok &= Expect(!ec, "create integration models dir");
		setenv("HOWDY_USER_MODELS_DIR", models_dir.c_str(), 1);

		auto run_boundary_aware_clear = [&]() -> int {
			std::array<std::string, 3> arguments{"howdy-clear", "alice", "-y"};
			std::array<char *, 3>      argv{arguments[0].data(), arguments[1].data(),
			                                arguments[2].data()};
			std::istringstream         input_stream;
			std::ostringstream         output_stream;
			StreamRedirect             redirect(std::cin, input_stream.rdbuf(), std::cout,
			                                    output_stream.rdbuf());
			return howdy::native::clear_internal::ClearMainWithValidationRoot(
			    static_cast<int>(argv.size()), argv.data(), {temp_root});
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

		fs::remove_all(temp_root, ec);
		if (saved_models_dir.has_value()) {
			setenv("HOWDY_USER_MODELS_DIR", saved_models_dir->c_str(), 1);
		} else {
			unsetenv("HOWDY_USER_MODELS_DIR");
		}
		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= MissingUserReturnsWithoutCallbacks();
	ok &= PublicMissingUserReturnsError();
	ok &= IncompleteDependenciesAbortWithoutCallbacks();
	ok &= InspectionOutcomesPreserveMessages();
	ok &= RejectedConfirmationAborts();
	ok &= AcceptedConfirmationPassesSnapshot();
	ok &= YesFlagBypassesConfirmation();
	ok &= ClearOutcomesPreserveMessages();
	ok &= SuccessfulClearPreservesOutput();
	ok &= BoundaryAwareClearRemovesUnparseableModelFiles();
	return ok ? 0 : 1;
}
