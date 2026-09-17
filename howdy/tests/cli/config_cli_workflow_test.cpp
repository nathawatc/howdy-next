#include "cli/config/internal.hpp"
#include "cli/config_cli_test_support.hpp"
#include "test_support.hpp"

#include <array>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/wait.h>

namespace howdy::test::config_cli {

	using howdy::test::Expect;

	using howdy::native::config_internal::ConfigDependencies;
	using howdy::native::config_internal::ConfigEditSession;
	using howdy::native::config_internal::ConfigEditStatus;
	using howdy::native::config_internal::TempConfigCopy;

	namespace {

		constexpr std::string_view kConfigPath              = "/test/howdy/config.ini";
		constexpr std::string_view kTempPath                = "/tmp/howdy-config-test";
		constexpr std::string_view kWorkflowOriginalContent = "[core]\ndisabled = false\n";
		constexpr std::string_view kEditedContent           = "[core]\ndisabled = true\n";

		struct TestContext {
			std::vector<int>                     order;
			std::vector<std::filesystem::path>   removed_paths;
			std::string                          editor = "nano";
			std::string                          edited_content{kEditedContent};
			std::string                          validation_error;
			std::string                          installer_error;
			std::string                          installer_content;
			std::string                          installer_expected_content;
			std::filesystem::path                config_path = kConfigPath;
			std::filesystem::path                installer_path;
			howdy::native::ConfigPathCheckResult security  = {.ok = true};
			std::optional<TempConfigCopy>        temp_copy = TempConfigCopy{
			    .path = kTempPath, .original_content = std::string{kWorkflowOriginalContent}};
			howdy::native::InvokingIdentityResult invoking_identity = {
			    .status = howdy::native::InvokingIdentityStatus::kResolved,
			    .user   = howdy::native::InvokingUser{.uid = 1000, .gid = 1000, .name = "alice"},
			};
			int                 editor_status      = 0;
			int                 editor_ready_calls = 0;
			std::array<int, 12> calls{};
			bool                throw_from_select_editor   = false;
			bool                read_result                = true;
			bool                validation_result          = true;
			bool                content_matches            = false;
			bool                installer_result           = true;
			bool                installer_lock             = false;
			bool                installer_validate_runtime = true;
			bool                installer_expected_nonnull = false;
		};

		struct RunResult {
			int         exit_code;
			std::string output;
		};

		void Record(TestContext &context, int callback) {
			++context.calls[static_cast<std::size_t>(callback)];
			context.order.push_back(callback);
		}

		auto ResolveInvokingIdentity(void *raw) -> howdy::native::InvokingIdentityResult {
			auto &context = *static_cast<TestContext *>(raw);
			Record(context, 0);
			return context.invoking_identity;
		}

		auto SelectEditorPreference(void *raw) -> std::string {
			auto &context = *static_cast<TestContext *>(raw);
			Record(context, 1);
			if (context.throw_from_select_editor) {
				throw std::runtime_error("test editor exception");
			}
			return context.editor;
		}

		auto ResolvePath(void *raw) -> std::filesystem::path {
			auto &context = *static_cast<TestContext *>(raw);
			Record(context, 2);
			return context.config_path;
		}

		auto CheckSecurity(void *raw, const std::filesystem::path &path)
		    -> howdy::native::ConfigPathCheckResult {
			auto &context = *static_cast<TestContext *>(raw);
			Record(context, 3);
			if (path != context.config_path) {
				context.security = {.ok = false, .error_message = "wrong security path"};
			}
			return context.security;
		}

		auto CreateTemp(void *raw, const std::filesystem::path &path,
		                const std::optional<howdy::native::InvokingUser> &user)
		    -> std::optional<TempConfigCopy> {
			auto &context = *static_cast<TestContext *>(raw);
			Record(context, 4);
			if (path != context.config_path ||
			    user.has_value() != context.invoking_identity.user.has_value()) {
				return std::nullopt;
			}
			return context.temp_copy;
		}

		auto RunEditor(void *raw, const std::string &editor, const std::filesystem::path &path,
		               const std::optional<howdy::native::InvokingUser> &user) -> int {
			auto &context = *static_cast<TestContext *>(raw);
			Record(context, 5);
			if (editor != context.editor || path != kTempPath ||
			    user.has_value() != context.invoking_identity.user.has_value()) {
				return -1;
			}
			return context.editor_status;
		}

		auto ReadSnapshot(void *raw, const std::filesystem::path &path, std::string *content)
		    -> bool {
			auto &context = *static_cast<TestContext *>(raw);
			Record(context, 6);
			if (path != kTempPath || content == nullptr) {
				return false;
			}
			*content = context.edited_content;
			return context.read_result;
		}

		auto Validate(void *raw, const std::string &content, std::string *error) -> bool {
			auto &context = *static_cast<TestContext *>(raw);
			Record(context, 7);
			if (content != context.edited_content || error == nullptr) {
				return false;
			}
			*error = context.validation_error;
			return context.validation_result;
		}

		auto Matches(void *raw, const std::filesystem::path &path, const std::string &content)
		    -> bool {
			auto &context = *static_cast<TestContext *>(raw);
			Record(context, 8);
			return path == context.config_path && content == context.edited_content &&
			       context.content_matches;
		}

		auto Install(void *raw, const std::filesystem::path &path, const std::string &content,
		             std::string *error, bool lock, bool validate_runtime,
		             const std::string *expected) -> bool {
			auto &context = *static_cast<TestContext *>(raw);
			Record(context, 9);
			context.installer_path             = path;
			context.installer_content          = content;
			context.installer_lock             = lock;
			context.installer_validate_runtime = validate_runtime;
			context.installer_expected_nonnull = expected != nullptr;
			context.installer_expected_content = expected == nullptr ? "" : *expected;
			*error                             = context.installer_error;
			return context.installer_result;
		}

		void RemovePath(void *raw, const std::filesystem::path &path) {
			auto &context = *static_cast<TestContext *>(raw);
			Record(context, 10);
			context.removed_paths.push_back(path);
		}

		void EditorReady(void *raw) {
			auto &context = *static_cast<TestContext *>(raw);
			Record(context, 11);
			++context.editor_ready_calls;
		}

		auto DependenciesFor(TestContext &context) -> ConfigDependencies {
			return {
			    .context                           = &context,
			    .resolve_invoking_identity         = ResolveInvokingIdentity,
			    .select_editor_preference          = SelectEditorPreference,
			    .resolve_config_path               = ResolvePath,
			    .check_secure_config_path          = CheckSecurity,
			    .create_temp_copy                  = CreateTemp,
			    .run_editor                        = RunEditor,
			    .read_temp_config_snapshot         = ReadSnapshot,
			    .validate_config_content           = Validate,
			    .file_content_matches              = Matches,
			    .replace_config_content_atomically = Install,
			    .remove_if_exists                  = RemovePath,
			};
		}

		void RemoveDependency(ConfigDependencies &dependencies, std::size_t missing) {
			switch (missing) {
				case 0:
					dependencies.resolve_invoking_identity = nullptr;
					break;
				case 1:
					dependencies.select_editor_preference = nullptr;
					break;
				case 2:
					dependencies.resolve_config_path = nullptr;
					break;
				case 3:
					dependencies.check_secure_config_path = nullptr;
					break;
				case 4:
					dependencies.create_temp_copy = nullptr;
					break;
				case 5:
					dependencies.run_editor = nullptr;
					break;
				case 6:
					dependencies.read_temp_config_snapshot = nullptr;
					break;
				case 7:
					dependencies.validate_config_content = nullptr;
					break;
				case 8:
					dependencies.file_content_matches = nullptr;
					break;
				case 9:
					dependencies.replace_config_content_atomically = nullptr;
					break;
				case 10:
					dependencies.remove_if_exists = nullptr;
					break;
				default:
					break;
			}
		}

		auto RunConfig(const ConfigDependencies &dependencies) -> RunResult {
			std::ostringstream output;
			ScopedStreamBuffer stdout_guard(std::cout, output.rdbuf());
			const int          exit_code =
			    howdy::native::config_internal::ConfigMainWithDependencies({}, dependencies);
			return {.exit_code = exit_code, .output = output.str()};
		}

		auto ExpectRemovedOnce(const TestContext &context, const std::string &message) -> bool {
			return Expect(context.removed_paths == std::vector<std::filesystem::path>{kTempPath},
			              message);
		}

		auto ExpectInstallerArguments(const TestContext &context, const std::string &message)
		    -> bool {
			return Expect(context.installer_path == kConfigPath &&
			                  context.installer_content == kEditedContent &&
			                  context.installer_lock && !context.installer_validate_runtime &&
			                  context.installer_expected_nonnull &&
			                  context.installer_expected_content == kWorkflowOriginalContent,
			              message);
		}

		auto StdoutRestoresAfterCallbackException() -> bool {
			bool               ok = true;
			std::ostringstream restored_output;
			ScopedStreamBuffer outer_stdout_guard(std::cout, restored_output.rdbuf());
			auto              *original_stdout = std::cout.rdbuf();
			TestContext        context;
			context.throw_from_select_editor = true;
			bool exception_observed          = false;
			try {
				(void)RunConfig(DependenciesFor(context));
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
				TestContext context;
				context.invoking_identity = {.status = identity_status};
				const auto result         = RunConfig(DependenciesFor(context));
				ok &= Expect(result.exit_code == 1 && result.output == expected_output,
				             "invalid invoking identity aborts config edit");
				ok &= Expect(context.order == std::vector{0},
				             "invalid invoking identity launches no editor workflow callback");
				ok &= Expect(context.editor_ready_calls == 0,
				             "invalid invoking identity skips editor-ready callback");
			}
			return ok;
		}

	}  // namespace

	auto RunConfigCliCallbackExceptionTest() -> bool {
		return StdoutRestoresAfterCallbackException();
	}

	auto RunConfigCliWorkflowTests() -> bool {
		bool ok = true;

		{
			TestContext             context;
			const ConfigEditSession session(DependenciesFor(context));
			const auto result = session.Run({.context = &context, .editor_ready = EditorReady});
			ok &= Expect(result.status == ConfigEditStatus::kOk,
			             "temporary dependencies session succeeds");
			ok &= Expect(result.error.empty() && result.temp_path == kTempPath &&
			                 result.editor == context.editor,
			             "temporary dependencies result matches success path");
			ok &= Expect(context.order == std::vector{0, 1, 2, 3, 4, 11, 5, 6, 7, 8, 9, 10},
			             "temporary dependencies invokes callbacks once in order");
			ok &= Expect(context.editor_ready_calls == 1,
			             "temporary dependencies invokes ready callback");
			ok &= ExpectRemovedOnce(context, "temporary dependencies removes temp once");
			ok &= ExpectInstallerArguments(context,
			                               "temporary dependencies installer arguments exact");
		}

		{
			TestContext             context;
			const ConfigEditSession session(ConfigDependencies{});
			const auto result = session.Run({.context = &context, .editor_ready = EditorReady});
			ok &= Expect(result.status == ConfigEditStatus::kDependenciesUnavailable,
			             "empty dependencies session reports unavailable dependencies");
			ok &= Expect(context.order.empty(), "empty dependencies session invokes no callbacks");
		}

		for (std::size_t missing = 0; missing < 11; ++missing) {
			TestContext context;
			auto        dependencies = DependenciesFor(context);
			RemoveDependency(dependencies, missing);

			const ConfigEditSession session(dependencies);
			const auto result = session.Run({.context = &context, .editor_ready = EditorReady});
			ok &= Expect(result.status == ConfigEditStatus::kDependenciesUnavailable,
			             "incomplete dependencies session reports unavailable dependencies");
			ok &= Expect(result.error.empty() && result.temp_path.empty() && result.editor.empty(),
			             "incomplete dependencies result has no side effects");
			ok &= Expect(context.order.empty(),
			             "incomplete dependencies session invokes no callbacks");
			ok &= Expect(context.editor_ready_calls == 0,
			             "incomplete dependencies session skips ready callback");
		}

		for (std::size_t missing = 0; missing < 11; ++missing) {
			TestContext context;
			auto        dependencies = DependenciesFor(context);
			RemoveDependency(dependencies, missing);
			const auto result = RunConfig(dependencies);
			ok &= Expect(result.exit_code == 1 && result.output.empty(),
			             "null dependency aborts silently");
			ok &= Expect(context.order.empty(), "null dependency invokes no callbacks");
		}

		ok &= InvokingIdentityFailuresAbortEdit();

		{
			TestContext context;
			context.editor    = {};
			const auto result = RunConfig(DependenciesFor(context));
			ok &= Expect(result.exit_code == 1 &&
			                 result.output == "Error: No suitable text editor found.\n"
			                                  "Set EDITOR to an executable name or path, or "
			                                  "install one of: micro, nano, vi.\n",
			             "no editor output exact");
			ok &= Expect(context.order == std::vector{0, 1}, "no editor stops before config path");
		}

		{
			TestContext context;
			context.security  = {.ok = false, .error_message = "Config path is insecure"};
			const auto result = RunConfig(DependenciesFor(context));
			ok &= Expect(result.exit_code == 1 && result.output == "Config path is insecure\n",
			             "unsafe path output exact");
			ok &= Expect(context.order == std::vector{0, 1, 2, 3},
			             "unsafe path stops before temp copy");
		}

		{
			TestContext context;
			context.temp_copy = std::nullopt;
			const auto result = RunConfig(DependenciesFor(context));
			ok &= Expect(result.exit_code == 1 &&
			                 result.output == "Failed to prepare a temporary config copy\n",
			             "temp-copy failure output exact");
			ok &= Expect(context.order == std::vector{0, 1, 2, 3, 4},
			             "temp failure stops before editor");
		}

		{
			TestContext context;
			context.editor_status = howdy::native::config_internal::kEditorUnavailableRunResult;
			const ConfigEditSession session(DependenciesFor(context));
			const auto result = session.Run({.context = &context, .editor_ready = EditorReady});
			ok &= Expect(result.status == ConfigEditStatus::kEditorUnavailable,
			             "editor unavailable launch report maps to unavailable status");
			ok &= ExpectRemovedOnce(context, "editor unavailable removes temp once");
		}

		{
			TestContext context;
			context.editor_status = -1;
			const auto result     = RunConfig(DependenciesFor(context));
			ok &= Expect(result.exit_code == 1 &&
			                 result.output == "Editing config.ini\nFailed to launch editor\n",
			             "editor launch failure output exact");
			ok &= ExpectRemovedOnce(context, "launch failure removes temp once");
			ok &= Expect(context.order == std::vector{0, 1, 2, 3, 4, 5, 10},
			             "launch failure stops after cleanup");
		}

		for (const int status : {W_EXITCODE(2, 0), 15}) {
			TestContext context;
			context.editor_status = status;
			const auto result     = RunConfig(DependenciesFor(context));
			ok &= Expect(result.exit_code == 1 &&
			                 result.output == "Editing config.ini\n"
			                                  "Editor exited unsuccessfully; config not updated\n",
			             "unsuccessful editor output exact");
			ok &= ExpectRemovedOnce(context, "unsuccessful editor removes temp once");
			ok &= Expect(context.calls[6] == 0, "unsuccessful editor skips later callbacks");
		}

		{
			TestContext context;
			context.read_result = false;
			const auto result   = RunConfig(DependenciesFor(context));
			ok &=
			    Expect(result.exit_code == 1 &&
			               result.output == "Editing config.ini\nFailed to install edited config\n",
			           "snapshot failure output exact");
			ok &= ExpectRemovedOnce(context, "snapshot failure removes temp once");
			ok &= Expect(context.calls[7] == 0 && context.calls[8] == 0 && context.calls[9] == 0,
			             "snapshot failure skips later callbacks");
		}

		for (const auto &[error, expected] : std::array<std::pair<std::string, std::string>, 3>{
		         std::pair{"Updated config is invalid",
		                   "Editing config.ini\nEdited config is invalid and was not "
		                   "installed: /tmp/howdy-config-test\n"},
		         std::pair{"Invalid timeout", "Editing config.ini\nInvalid timeout\n"},
		         std::pair{"", "Editing config.ini\nFailed to install edited config\n"}}) {
			TestContext context;
			context.validation_result = false;
			context.validation_error  = error;
			const auto result         = RunConfig(DependenciesFor(context));
			ok &= Expect(result.exit_code == 1 && result.output == expected,
			             "invalid edit output exact");
			ok &= Expect(context.removed_paths.empty(), "invalid edit preserves temp");
			ok &= Expect(context.calls[8] == 0 && context.calls[9] == 0,
			             "invalid edit skips install");
		}

		{
			TestContext context;
			context.content_matches = true;
			const auto result       = RunConfig(DependenciesFor(context));
			ok &= Expect(result.exit_code == 0 &&
			                 result.output == "Editing config.ini\nNo config changes made\n",
			             "no-op output exact");
			ok &= ExpectRemovedOnce(context, "no-op removes temp once");
			ok &= Expect(context.calls[9] == 0, "no-op skips installer");
		}

		for (const auto &[error, expected] : std::array<std::pair<std::string, std::string>, 2>{
		         std::pair{"Config changed while editor was open",
		                   "Editing config.ini\nConfig changed while editor was open\n"},
		         std::pair{"", "Editing config.ini\nFailed to install edited config\n"}}) {
			TestContext context;
			context.installer_result = false;
			context.installer_error  = error;
			const auto result        = RunConfig(DependenciesFor(context));
			ok &= Expect(result.exit_code == 1 && result.output == expected,
			             "install failure output exact");
			ok &= ExpectRemovedOnce(context, "install failure removes temp once");
			ok &= ExpectInstallerArguments(context, "install failure arguments exact");
		}

		{
			TestContext context;
			const auto  result = RunConfig(DependenciesFor(context));
			ok &= Expect(result.exit_code == 0 &&
			                 result.output == "Editing config.ini\nConfig updated\n",
			             "success output exact");
			ok &= Expect(context.order == std::vector{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10},
			             "success invokes callbacks once in order");
			ok &= ExpectRemovedOnce(context, "success removes temp once");
			ok &= ExpectInstallerArguments(context, "success installer arguments exact");
		}

		return ok;
	}

}  // namespace howdy::test::config_cli
