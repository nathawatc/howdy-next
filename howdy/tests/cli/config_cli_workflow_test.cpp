#include "cli/config/internal.hpp"
#include "cli/config_cli_test_support.hpp"
#include "test_support.hpp"

#include <array>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <sys/wait.h>

namespace howdy::test::config_cli {

	using howdy::test::Expect;
	using howdy::test::ReadFile;
	using howdy::test::WriteFile;

	using howdy::native::config_internal::ConfigEditDependencies;
	using howdy::native::config_internal::ConfigEditSession;
	using howdy::native::config_internal::ConfigEditStatus;

	namespace {

		namespace fs = std::filesystem;

		constexpr std::string_view kWorkflowOriginalContent = "[core]\ndisabled = false\n";
		constexpr std::string_view kEditedContent           = "[core]\ndisabled = true\n";

		struct TestContext {
			std::string                           editor = "nano";
			std::string                           edited_content{kEditedContent};
			howdy::native::InvokingIdentityResult invoking_identity = {
			    .status = howdy::native::InvokingIdentityStatus::kResolved,
			    .user   = howdy::native::InvokingUser{.uid  = geteuid(),
			                                          .gid  = getegid(),
			                                          .name = "alice"},
			};
			int                                        editor_status            = 0;
			int                                        editor_ready_calls       = 0;
			int                                        editor_calls             = 0;
			bool                                       delete_temp_in_editor    = false;
			bool                                       throw_from_select_editor = false;
			fs::path                                   recorded_temp_path;
			std::string                                recorded_editor;
			std::optional<howdy::native::InvokingUser> recorded_user;
			std::function<void(const fs::path &)>      on_editor;
		};

		struct TestDirectoryFixture {
			fs::path temp_root;
			fs::path config_path;

			TestDirectoryFixture() {
				const auto temp_template = fs::temp_directory_path() / "howdy-workflow-test-XXXXXX";
				std::string       template_str = temp_template.string();
				std::vector<char> writable(template_str.begin(), template_str.end());
				writable.push_back('\0');
				const char *created = mkdtemp(writable.data());
				if (created == nullptr) {
					throw std::runtime_error("failed to create test directory");
				}
				temp_root = fs::path(created);
				chmod(temp_root.c_str(), 0700);
				config_path = temp_root / "config.ini";
				WriteFile(config_path, std::string(kWorkflowOriginalContent));
				chmod(config_path.c_str(), 0644);
			}

			~TestDirectoryFixture() {
				std::error_code ec;
				fs::remove_all(temp_root, ec);
			}
		};

		struct RunResult {
			int         exit_code;
			std::string output;
		};

		auto ResolveInvokingIdentity(void *raw) -> howdy::native::InvokingIdentityResult {
			auto &context = *static_cast<TestContext *>(raw);
			return context.invoking_identity;
		}

		auto SelectEditorPreference(void *raw) -> std::string {
			auto &context = *static_cast<TestContext *>(raw);
			if (context.throw_from_select_editor) {
				throw std::runtime_error("test editor exception");
			}
			return context.editor;
		}

		auto RunEditor(void *raw, const std::string &editor, const fs::path &temp_path,
		               const std::optional<howdy::native::InvokingUser> &user) -> int {
			auto &context              = *static_cast<TestContext *>(raw);
			context.recorded_editor    = editor;
			context.recorded_temp_path = temp_path;
			context.recorded_user      = user;
			++context.editor_calls;

			if (context.on_editor) {
				context.on_editor(temp_path);
			} else if (context.delete_temp_in_editor) {
				std::error_code ec;
				fs::remove(temp_path, ec);
			} else {
				WriteFile(temp_path, context.edited_content);
			}
			return context.editor_status;
		}

		void EditorReady(void *raw) {
			auto &context = *static_cast<TestContext *>(raw);
			++context.editor_ready_calls;
		}

		auto DependenciesFor(TestContext &context, const fs::path &temp_root)
		    -> ConfigEditDependencies {
			return {
			    .context                   = &context,
			    .resolve_invoking_identity = ResolveInvokingIdentity,
			    .select_editor_preference  = SelectEditorPreference,
			    .run_editor                = RunEditor,
			    .validation_root           = {temp_root},
			};
		}

		auto RunConfig(const ConfigEditDependencies &dependencies, const fs::path &config_path)
		    -> RunResult {
			ScopedEnvironmentVariable config_env("HOWDY_CONFIG", config_path.string());
			std::ostringstream        output;
			ScopedStreamBuffer        stdout_guard(std::cout, output.rdbuf());
			const int                 exit_code =
			    howdy::native::config_internal::ConfigMainWithDependencies({}, dependencies);
			return {.exit_code = exit_code, .output = output.str()};
		}

		auto StdoutRestoresAfterCallbackException() -> bool {
			bool                 ok = true;
			std::ostringstream   restored_output;
			ScopedStreamBuffer   outer_stdout_guard(std::cout, restored_output.rdbuf());
			auto                *original_stdout = std::cout.rdbuf();
			TestDirectoryFixture fixture;
			TestContext          context;
			context.throw_from_select_editor = true;
			bool exception_observed          = false;
			try {
				(void)RunConfig(DependenciesFor(context, fixture.temp_root), fixture.config_path);
			} catch (const std::runtime_error &) {
				exception_observed = true;
			}
			ok &= Expect(exception_observed, "editor callback exception escapes test runner");
			ok &= Expect(std::cout.rdbuf() == original_stdout,
			             "stdout buffer restores after callback exception");
			std::cout << "stdout-restored-marker";
			ok &= Expect(restored_output.str() == "stdout-restored-marker",
			             "stdout marker reaches restored buffer");
			return ok;
		}

		auto InvokingIdentityFailuresAbortEdit() -> bool {
			bool ok = true;
			for (const auto &[identity_status, expected_output] :
			     std::array<std::pair<howdy::native::InvokingIdentityStatus, std::string>, 2>{
			         std::pair{howdy::native::InvokingIdentityStatus::kInvalid,
			                   "Invalid privilege-wrapper identity; config edit aborted\n"},
			         std::pair{howdy::native::InvokingIdentityStatus::kConflicting,
			                   "Conflicting privilege-wrapper identity; config edit aborted\n"}}) {
				TestDirectoryFixture fixture;
				TestContext          context;
				context.invoking_identity = {.status = identity_status};
				const auto result =
				    RunConfig(DependenciesFor(context, fixture.temp_root), fixture.config_path);
				ok &= Expect(result.exit_code == 1 && result.output == expected_output,
				             "invalid invoking identity aborts config edit");
				ok &= Expect(context.editor_calls == 0,
				             "invalid invoking identity launches no editor callback");
				ok &= Expect(context.editor_ready_calls == 0,
				             "invalid invoking identity skips editor-ready callback");
			}
			return ok;
		}

		auto MissingDependenciesFailClosed() -> bool {
			bool ok = true;
			{
				TestContext             context;
				const ConfigEditSession session(ConfigEditDependencies{});
				const auto result = session.Run({.context = &context, .editor_ready = EditorReady});
				ok &= Expect(result.status == ConfigEditStatus::kDependenciesUnavailable,
				             "empty dependencies session reports unavailable dependencies");
				ok &= Expect(context.editor_calls == 0,
				             "empty dependencies session invokes no editor");
			}

			for (std::size_t missing = 0; missing < 3; ++missing) {
				TestDirectoryFixture fixture;
				TestContext          context;
				auto                 dependencies = DependenciesFor(context, fixture.temp_root);
				switch (missing) {
					case 0:
						dependencies.resolve_invoking_identity = nullptr;
						break;
					case 1:
						dependencies.select_editor_preference = nullptr;
						break;
					case 2:
						dependencies.run_editor = nullptr;
						break;
					default:
						break;
				}

				const ConfigEditSession session(dependencies);
				const auto result = session.Run({.context = &context, .editor_ready = EditorReady});
				ok &= Expect(result.status == ConfigEditStatus::kDependenciesUnavailable,
				             "incomplete dependencies session reports unavailable dependencies");
				ok &= Expect(result.error.empty() && result.temp_path.empty() &&
				                 result.editor.empty(),
				             "incomplete dependencies result has no side effects");
				ok &= Expect(context.editor_calls == 0,
				             "incomplete dependencies session invokes no callbacks");
				ok &= Expect(context.editor_ready_calls == 0,
				             "incomplete dependencies session skips ready callback");

				const auto run_result = RunConfig(dependencies, fixture.config_path);
				ok &= Expect(run_result.exit_code == 1 && run_result.output.empty(),
				             "missing dependency aborts silently from ConfigMainWithDependencies");
			}
			return ok;
		}

		auto EditorSelectionAndSecurityWorkflow() -> bool {
			bool ok = true;
			{
				TestDirectoryFixture fixture;
				TestContext          context;
				context.editor = {};
				const auto result =
				    RunConfig(DependenciesFor(context, fixture.temp_root), fixture.config_path);
				ok &= Expect(result.exit_code == 1 &&
				                 result.output == "Error: No suitable text editor found.\n"
				                                  "Set EDITOR to an executable name or path, or "
				                                  "install one of: micro, nano, vi.\n",
				             "no editor output exact");
				ok &= Expect(context.editor_calls == 0, "no editor stops before launching");
			}

			{
				TestDirectoryFixture fixture;
				TestContext          context;
				chmod(fixture.config_path.c_str(), 0777);
				const auto result =
				    RunConfig(DependenciesFor(context, fixture.temp_root), fixture.config_path);
				ok &= Expect(result.exit_code == 1, "unsafe path fails");
				ok &= Expect(result.output.contains("must not be group or world-writable") ||
				                 result.output.contains("Config file"),
				             "unsafe path output contains security error");
				ok &= Expect(context.editor_calls == 0, "unsafe path stops before editor");
			}
			return ok;
		}

		auto EditorExecutionFailures() -> bool {
			bool ok = true;
			{
				TestDirectoryFixture fixture;
				TestContext          context;
				context.editor_status = howdy::native::config_internal::kEditorUnavailableRunResult;
				ScopedEnvironmentVariable config_env("HOWDY_CONFIG", fixture.config_path.string());
				const ConfigEditSession   session(DependenciesFor(context, fixture.temp_root));
				const auto result = session.Run({.context = &context, .editor_ready = EditorReady});
				ok &= Expect(result.status == ConfigEditStatus::kEditorUnavailable,
				             "editor unavailable launch report maps to unavailable status");
				ok &= Expect(!context.recorded_temp_path.empty() &&
				                 !fs::exists(context.recorded_temp_path),
				             "editor unavailable removes temp once");
			}

			{
				TestDirectoryFixture fixture;
				TestContext          context;
				context.editor_status = -1;
				const auto result =
				    RunConfig(DependenciesFor(context, fixture.temp_root), fixture.config_path);
				ok &= Expect(result.exit_code == 1 &&
				                 result.output == "Editing config.ini\nFailed to launch editor\n",
				             "editor launch failure output exact");
				ok &= Expect(!context.recorded_temp_path.empty() &&
				                 !fs::exists(context.recorded_temp_path),
				             "launch failure removes temp once");
			}

			for (const int status : {W_EXITCODE(2, 0), 15}) {
				TestDirectoryFixture fixture;
				TestContext          context;
				context.editor_status = status;
				const auto result =
				    RunConfig(DependenciesFor(context, fixture.temp_root), fixture.config_path);
				ok &= Expect(result.exit_code == 1 &&
				                 result.output ==
				                     "Editing config.ini\n"
				                     "Editor exited unsuccessfully; config not updated\n",
				             "unsuccessful editor output exact");
				ok &= Expect(!context.recorded_temp_path.empty() &&
				                 !fs::exists(context.recorded_temp_path),
				             "unsuccessful editor removes temp once");
			}

			{
				TestDirectoryFixture fixture;
				TestContext          context;
				context.delete_temp_in_editor = true;
				const auto result =
				    RunConfig(DependenciesFor(context, fixture.temp_root), fixture.config_path);
				ok &= Expect(result.exit_code == 1 && result.output ==
				                                          "Editing config.ini\nFailed to install "
				                                          "edited config\n",
				             "snapshot failure output exact");
			}
			return ok;
		}

		auto InvalidEditPreservesTempFile() -> bool {
			bool ok = true;
			for (const auto &[error_input, expected_suffix] :
			     std::array<std::pair<std::string, std::string>, 2>{
			         std::pair{"[core\n", "Editing config.ini\nEdited config is invalid and was "
			                              "not installed: "},
			         std::pair{"[video]\ntimeout = 0\n",
			                   "Editing config.ini\nInvalid config value for timeout=\"0\": "
			                   "expected integer range 1..300\n"}}) {
				TestDirectoryFixture fixture;
				TestContext          context;
				context.edited_content = error_input;
				const auto result =
				    RunConfig(DependenciesFor(context, fixture.temp_root), fixture.config_path);
				ok &= Expect(result.exit_code == 1, "invalid edit exits with failure code");
				if (error_input == "[core\n") {
					ok &= Expect(result.output.starts_with(expected_suffix),
					             "invalid edit reports recovery prefix exact");
				} else {
					ok &= Expect(result.output == expected_suffix, "validation error output exact");
				}
				ok &= Expect(!context.recorded_temp_path.empty(), "temp path recorded");
				ok &= Expect(fs::exists(context.recorded_temp_path),
				             "invalid edit PRESERVES temp file");
				ok &= Expect(ReadFile(context.recorded_temp_path) == error_input,
				             "preserved temp file retains user edited content");
				std::error_code ec;
				fs::remove(context.recorded_temp_path, ec);
			}
			return ok;
		}

		auto SuccessfulAndUnchangedWorkflows() -> bool {
			bool ok = true;
			{
				TestDirectoryFixture      fixture;
				TestContext               context;
				const ConfigEditSession   session(DependenciesFor(context, fixture.temp_root));
				ScopedEnvironmentVariable config_env("HOWDY_CONFIG", fixture.config_path.string());
				const auto result = session.Run({.context = &context, .editor_ready = EditorReady});
				ok &= Expect(result.status == ConfigEditStatus::kOk,
				             "direct dependencies session succeeds");
				ok &= Expect(result.error.empty() && !result.temp_path.empty() &&
				                 result.editor == context.editor,
				             "direct dependencies result matches success path");
				ok &= Expect(context.editor_calls == 1 && context.editor_ready_calls == 1,
				             "editor callbacks invoked once");
				ok &= Expect(!fs::exists(result.temp_path), "successful session removes temp file");
				ok &= Expect(ReadFile(fixture.config_path) == kEditedContent,
				             "successful session updates config file");
			}

			{
				TestDirectoryFixture fixture;
				TestContext          context;
				context.edited_content = std::string(kWorkflowOriginalContent);
				const auto result =
				    RunConfig(DependenciesFor(context, fixture.temp_root), fixture.config_path);
				ok &= Expect(result.exit_code == 0 &&
				                 result.output == "Editing config.ini\nNo config changes made\n",
				             "no-op output exact");
				ok &= Expect(!context.recorded_temp_path.empty() &&
				                 !fs::exists(context.recorded_temp_path),
				             "no-op removes temp once");
				ok &= Expect(ReadFile(fixture.config_path) == kWorkflowOriginalContent,
				             "no-op leaves config unchanged");
			}

			{
				TestDirectoryFixture fixture;
				TestContext          context;
				context.on_editor = [&](const fs::path &temp_path) -> void {
					WriteFile(fixture.config_path, "[core]\ndisabled = true\n");
					WriteFile(temp_path, "[core]\nother = 1\n");
				};
				const auto result =
				    RunConfig(DependenciesFor(context, fixture.temp_root), fixture.config_path);
				ok &= Expect(result.exit_code == 1 &&
				                 result.output == "Editing config.ini\n"
				                                  "Config changed while editing; not installing "
				                                  "stale edited config\n",
				             "install failure output exact");
				ok &= Expect(!context.recorded_temp_path.empty() &&
				                 !fs::exists(context.recorded_temp_path),
				             "install failure removes temp once");
			}

			{
				TestDirectoryFixture fixture;
				TestContext          context;
				const auto           result =
				    RunConfig(DependenciesFor(context, fixture.temp_root), fixture.config_path);
				ok &= Expect(result.exit_code == 0 &&
				                 result.output == "Editing config.ini\nConfig updated\n",
				             "success output exact");
				ok &= Expect(!context.recorded_temp_path.empty() &&
				                 !fs::exists(context.recorded_temp_path),
				             "success removes temp once");
				ok &= Expect(ReadFile(fixture.config_path) == kEditedContent,
				             "success updates config file");
				ok &= Expect(context.editor_calls == 1, "editor invoked once");
				ok &=
				    Expect(context.recorded_editor == "nano", "editor preference passed to runner");
				ok &= Expect(context.recorded_user.has_value() &&
				                 context.recorded_user->name == "alice",
				             "invoking user passed to runner");
			}
			return ok;
		}

	}  // namespace

	auto RunConfigCliCallbackExceptionTest() -> bool {
		return StdoutRestoresAfterCallbackException();
	}

	auto RunConfigCliWorkflowTests() -> bool {
		bool ok = true;
		ok &= MissingDependenciesFailClosed();
		ok &= InvokingIdentityFailuresAbortEdit();
		ok &= EditorSelectionAndSecurityWorkflow();
		ok &= EditorExecutionFailures();
		ok &= InvalidEditPreservesTempFile();
		ok &= SuccessfulAndUnchangedWorkflows();
		return ok;
	}

}  // namespace howdy::test::config_cli
