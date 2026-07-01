#include "cli/clear_cli.hpp"
#include "cli/clear_internal.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <vector>

#include <sys/wait.h>

namespace {

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

	auto expect(bool condition, const std::string &message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << "\n";
			return false;
		}
		return true;
	}

	auto write_file(const std::filesystem::path &path, const std::string &content) -> bool {
		std::ofstream out(path);
		if (!out.is_open()) {
			return false;
		}
		out << content;
		return out.good();
	}

	auto valid_snapshot() -> howdy::native::UserModelFileSnapshot {
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

	auto success_context() -> ClearCliTestContext {
		ClearCliTestContext context;
		context.inspect_result = {
		    .status   = howdy::native::UserModelStatus::kOk,
		    .snapshot = valid_snapshot(),
		};
		context.clear_result = {.status = howdy::native::UserModelStatus::kOk};
		return context;
	}

	auto inspect_callback(void *raw_context, const std::string &user)
	    -> howdy::native::UserModelInspectResult {
		auto *context = static_cast<ClearCliTestContext *>(raw_context);
		++context->inspect_calls;
		context->inspected_user = user;
		return context->inspect_result;
	}

	auto clear_callback(void *raw_context, const std::string &user,
	                    const howdy::native::UserModelFileSnapshot &expected_snapshot)
	    -> howdy::native::UserModelMutationResult {
		auto *context = static_cast<ClearCliTestContext *>(raw_context);
		++context->clear_calls;
		context->cleared_user      = user;
		context->received_snapshot = expected_snapshot;
		return context->clear_result;
	}

	auto run_clear_with_dependencies(
	    std::vector<std::string>                                arguments,
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
		const int      result = howdy::native::clear_internal::clear_main_with_dependencies(
		    static_cast<int>(argv.size()), argv.data(), dependencies);
		return {result, output_stream.str()};
	}

	auto run_clear(ClearCliTestContext &context, std::vector<std::string> arguments,
	               const std::string &input = {}) -> std::pair<int, std::string> {
		return run_clear_with_dependencies(
		    std::move(arguments),
		    {
		        .context                               = &context,
		        .inspect_user_model_file               = inspect_callback,
		        .clear_user_model_entries_if_unchanged = clear_callback,
		    },
		    input);
	}

	auto missing_user_returns_without_callbacks() -> bool {
		auto context          = success_context();
		auto [result, output] = run_clear(context, {"howdy-clear"});
		return expect(result == 1, "missing user returns 1") &&
		       expect(output.empty(), "missing user stays silent") &&
		       expect(context.inspect_calls == 0 && context.clear_calls == 0,
		              "missing user skips callbacks");
	}

	auto public_missing_user_terminates_process() -> bool {
		const pid_t child_pid = fork();
		if (!expect(child_pid >= 0, "fork public clear child")) {
			return false;
		}
		if (child_pid == 0) {
			auto                  command = std::to_array("howdy-clear");
			std::array<char *, 1> argv{command.data()};
			clear_main(1, argv.data());
			_exit(42);
		}

		int status = 0;
		if (!expect(waitpid(child_pid, &status, 0) == child_pid, "wait for public clear child")) {
			return false;
		}
		return expect(WIFEXITED(status) && WEXITSTATUS(status) == 1,
		              "public missing user terminates with status 1");
	}

	auto incomplete_dependencies_abort_without_callbacks() -> bool {
		auto context                                    = success_context();
		auto [null_inspect_result, null_inspect_output] = run_clear_with_dependencies(
		    {"howdy-clear", "alice"}, {
		                                  .context                               = &context,
		                                  .inspect_user_model_file               = nullptr,
		                                  .clear_user_model_entries_if_unchanged = clear_callback,
		                              });
		bool ok = true;
		ok &= expect(null_inspect_result == 1, "null inspect callback returns 1");
		ok &= expect(null_inspect_output.empty(), "null inspect callback stays silent");
		ok &= expect(context.inspect_calls == 0 && context.clear_calls == 0,
		             "null inspect callback skips callbacks");

		auto [null_clear_result, null_clear_output] = run_clear_with_dependencies(
		    {"howdy-clear", "alice"}, {
		                                  .context                               = &context,
		                                  .inspect_user_model_file               = inspect_callback,
		                                  .clear_user_model_entries_if_unchanged = nullptr,
		                              });
		ok &= expect(null_clear_result == 1, "null clear callback returns 1");
		ok &= expect(null_clear_output.empty(), "null clear callback stays silent");
		ok &= expect(context.inspect_calls == 0 && context.clear_calls == 0,
		             "null clear callback skips callbacks");
		return ok;
	}

	auto inspection_outcomes_preserve_messages() -> bool {
		bool ok = true;
		for (const auto &[status, error, snapshot, expected] : std::vector<
		         std::tuple<howdy::native::UserModelStatus, std::string,
		                    std::optional<howdy::native::UserModelFileSnapshot>, std::string>>{
		         {howdy::native::UserModelStatus::kNoModelDirectory,
		          {},
		          std::nullopt,
		          "No models created yet, can't clear them if they don't exist\n"},
		         {howdy::native::UserModelStatus::kNoModel,
		          {},
		          std::nullopt,
		          "alice has no models or they have been cleared already\n"},
		         {howdy::native::UserModelStatus::kInsecurePath, "storage failed", std::nullopt,
		          "storage failed\n"},
		         {howdy::native::UserModelStatus::kOk,
		          {},
		          std::nullopt,
		          "Failed to inspect user model file\n"},
		     }) {
			auto context           = success_context();
			context.inspect_result = {
			    .status = status, .error_message = error, .snapshot = snapshot};
			auto [result, output] = run_clear(context, {"howdy-clear", "alice", "-y"});
			ok &= expect(result == 1, "inspection outcome returns 1");
			ok &= expect(output == expected, "inspection outcome preserves message");
			ok &= expect(context.inspect_calls == 1 && context.clear_calls == 0,
			             "inspection outcome inspects once and skips clear");
		}
		return ok;
	}

	auto rejected_confirmation_aborts() -> bool {
		auto context          = success_context();
		auto [result, output] = run_clear(context, {"howdy-clear", "alice"}, "n\n");
		return expect(result == 1, "rejected confirmation returns 1") &&
		       expect(output == "This will clear all models for alice\n"
		                        "Do you want to continue [y/N]: "
		                        "\nInterpreting as a \"NO\", aborting\n",
		              "rejected confirmation preserves output") &&
		       expect(context.inspect_calls == 1 && context.clear_calls == 0,
		              "rejected confirmation inspects once and skips clear");
	}

	auto accepted_confirmation_passes_snapshot() -> bool {
		auto context          = success_context();
		auto [result, output] = run_clear(context, {"howdy-clear", "alice"}, "Y\n");
		const auto &snapshot  = context.received_snapshot;
		bool        ok        = true;
		ok &= expect(result == 0, "accepted confirmation returns 0");
		ok &= expect(context.inspect_calls == 1 && context.inspected_user == "alice",
		             "accepted confirmation inspects requested user once");
		ok &= expect(context.clear_calls == 1 && context.cleared_user == "alice",
		             "accepted confirmation clears requested user once");
		ok &= expect(snapshot.dev == 11 && snapshot.inode == 22 && snapshot.size == 33 &&
		                 snapshot.mtime_seconds == 44 && snapshot.mtime_nanosecs == 55 &&
		                 snapshot.ctime_seconds == 66 && snapshot.ctime_nanosecs == 77,
		             "accepted confirmation passes complete snapshot unchanged");
		ok &= expect(output == "This will clear all models for alice\n"
		                       "Do you want to continue [y/N]: \nModels cleared\n",
		             "accepted confirmation preserves output");
		return ok;
	}

	auto yes_flag_bypasses_confirmation() -> bool {
		auto context          = success_context();
		auto [result, output] = run_clear(context, {"howdy-clear", "alice", "-y"});
		return expect(result == 0, "-y returns 0") &&
		       expect(output == "\nModels cleared\n", "-y preserves output without prompt") &&
		       expect(context.inspect_calls == 1 && context.clear_calls == 1,
		              "-y inspects and clears once");
	}

	auto clear_outcomes_preserve_messages() -> bool {
		bool ok = true;
		for (const auto &[status, error, expected] :
		     std::vector<std::tuple<howdy::native::UserModelStatus, std::string, std::string>>{
		         {howdy::native::UserModelStatus::kNoModelDirectory,
		          {},
		          "No models created yet, can't clear them if they don't exist\n"},
		         {howdy::native::UserModelStatus::kNoModel,
		          {},
		          "alice has no models or they have been cleared already\n"},
		         {howdy::native::UserModelStatus::kModelChanged, "model changed",
		          "model changed\n"},
		     }) {
			auto context          = success_context();
			context.clear_result  = {.status = status, .error_message = error};
			auto [result, output] = run_clear(context, {"howdy-clear", "alice", "-y"});
			ok &= expect(result == 1, "clear outcome returns 1");
			ok &= expect(output == expected, "clear outcome preserves message");
			ok &= expect(context.inspect_calls == 1 && context.clear_calls == 1,
			             "clear outcome invokes callbacks once");
		}
		return ok;
	}

	auto successful_clear_preserves_output() -> bool {
		auto context          = success_context();
		auto [result, output] = run_clear(context, {"howdy-clear", "alice", "-y"});
		return expect(result == 0, "successful clear returns 0") &&
		       expect(output == "\nModels cleared\n", "successful clear preserves output");
	}

	auto public_clear_removes_unparseable_model_files() -> bool {
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
		if (!expect(temp_path != nullptr, "create unique integration temp dir")) {
			return false;
		}

		bool            ok         = true;
		const fs::path  temp_root  = temp_path;
		const auto      models_dir = temp_root / "models";
		const auto      model_path = models_dir / "alice.dat";
		std::error_code ec;

		fs::create_directories(models_dir, ec);
		ok &= expect(!ec, "create integration models dir");
		setenv("HOWDY_USER_MODELS_DIR", models_dir.c_str(), 1);

		auto run_public_clear = [&]() {
			std::array<std::string, 3> arguments{"howdy-clear", "alice", "-y"};
			std::array<char *, 3>      argv{arguments[0].data(), arguments[1].data(),
			                                arguments[2].data()};
			std::istringstream         input_stream;
			std::ostringstream         output_stream;
			StreamRedirect             redirect(std::cin, input_stream.rdbuf(), std::cout,
			                                    output_stream.rdbuf());
			return clear_main(static_cast<int>(argv.size()), argv.data());
		};

		ok &= expect(write_file(model_path, "not-json"), "write malformed model JSON");
		ok &= expect(run_public_clear() == 0, "public clear removes malformed JSON");
		ok &= expect(!fs::exists(model_path), "malformed JSON model deleted");

		std::string oversized_json =
		    R"json([{"id":1,"label":"large","data":[[0.1]],"padding":")json";
		oversized_json.append((1024 * 1024) + 1, 'x');
		oversized_json += "\"}]";
		ok &= expect(write_file(model_path, oversized_json), "write oversized model JSON");
		ok &= expect(run_public_clear() == 0, "public clear removes oversized JSON");
		ok &= expect(!fs::exists(model_path), "oversized JSON model deleted");

		ok &= expect(write_file(model_path, R"({"id":1})"), "write wrong-shape model JSON");
		ok &= expect(run_public_clear() == 0, "public clear removes wrong-shape JSON");
		ok &= expect(!fs::exists(model_path), "wrong-shape JSON model deleted");

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
	ok &= missing_user_returns_without_callbacks();
	ok &= public_missing_user_terminates_process();
	ok &= incomplete_dependencies_abort_without_callbacks();
	ok &= inspection_outcomes_preserve_messages();
	ok &= rejected_confirmation_aborts();
	ok &= accepted_confirmation_passes_snapshot();
	ok &= yes_flag_bypasses_confirmation();
	ok &= clear_outcomes_preserve_messages();
	ok &= successful_clear_preserves_output();
	ok &= public_clear_removes_unparseable_model_files();
	return ok ? 0 : 1;
}
