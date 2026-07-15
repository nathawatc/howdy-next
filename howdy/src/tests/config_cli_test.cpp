#include "cli/config_cli.hpp"
#include "cli/config_internal.hpp"
#include "config/config_limits.hpp"
#include "test_support.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

extern "C" auto real_initgroups(const char *user, gid_t group) -> int asm("__real_initgroups");
extern "C" auto wrap_initgroups(const char *user, gid_t group) -> int asm("__wrap_initgroups");

extern "C" auto wrap_initgroups(const char *user, gid_t group) -> int {
	// Unprivileged tests cannot call initgroups even when target identity is unchanged.
	if (group == getgid()) {
		return 0;
	}
	return real_initgroups(user, group);
}

namespace {

	using howdy::test::expect;

	using howdy::native::config_internal::ConfigDependencies;
	using howdy::native::config_internal::ConfigEditSession;
	using howdy::native::config_internal::ConfigEditStatus;
	using howdy::native::config_internal::TempConfigCopy;

	constexpr std::string_view kConfigPath      = "/test/howdy/config.ini";
	constexpr std::string_view kTempPath        = "/tmp/howdy-config-test";
	constexpr std::string_view kOriginalContent = "[core]\ndisabled = false\n";
	constexpr std::string_view kEditedContent   = "[core]\ndisabled = true\n";

	class ScopedStreamBuffer {
	public:
		ScopedStreamBuffer(std::ostream &stream, std::streambuf *replacement)
		    : stream_(stream)
		    , original_(stream.rdbuf(replacement)) {}

		ScopedStreamBuffer(const ScopedStreamBuffer &)                     = delete;
		auto operator=(const ScopedStreamBuffer &) -> ScopedStreamBuffer & = delete;

		~ScopedStreamBuffer() noexcept {
			stream_.rdbuf(original_);
		}

	private:
		std::ostream   &stream_;
		std::streambuf *original_;
	};

	class ScopedEnvironmentVariable {
	public:
		ScopedEnvironmentVariable(const char *name, const std::string &value)
		    : name_(name) {
			if (const char *current = std::getenv(name); current != nullptr) {
				original_ = current;
			}
			setenv(name_, value.c_str(), 1);
		}

		ScopedEnvironmentVariable(const ScopedEnvironmentVariable &)                     = delete;
		auto operator=(const ScopedEnvironmentVariable &) -> ScopedEnvironmentVariable & = delete;

		~ScopedEnvironmentVariable() noexcept {
			if (original_.has_value()) {
				setenv(name_, original_->c_str(), 1);
			} else {
				unsetenv(name_);
			}
		}

	private:
		const char                *name_;
		std::optional<std::string> original_;
	};

	struct TestContext {
		std::array<int, 12>                        calls{};
		std::vector<int>                           order;
		std::optional<howdy::native::InvokingUser> invoking_user =
		    howdy::native::InvokingUser{.uid = 1000, .gid = 1000, .name = "alice"};
		bool                                 allow_env_editor          = false;
		bool                                 throw_from_resolve_editor = false;
		std::string                          editor                    = "/usr/bin/nano";
		std::filesystem::path                config_path               = kConfigPath;
		howdy::native::ConfigPathCheckResult security                  = {.ok = true};
		std::optional<TempConfigCopy>        temp_copy =
		    TempConfigCopy{.path = kTempPath, .original_content = std::string{kOriginalContent}};
		int                                editor_status = 0;
		bool                               read_result   = true;
		std::string                        edited_content{kEditedContent};
		bool                               validation_result = true;
		std::string                        validation_error;
		bool                               content_matches  = false;
		bool                               installer_result = true;
		std::string                        installer_error;
		std::filesystem::path              installer_path;
		std::string                        installer_content;
		bool                               installer_lock             = false;
		bool                               installer_validate_runtime = true;
		bool                               installer_expected_nonnull = false;
		std::string                        installer_expected_content;
		std::vector<std::filesystem::path> removed_paths;
		int                                editor_ready_calls = 0;
		std::string                        editor_ready_editor;
	};

	struct RunResult {
		int         exit_code;
		std::string output;
	};

	void record(TestContext &context, int callback) {
		++context.calls[static_cast<std::size_t>(callback)];
		context.order.push_back(callback);
	}

	auto resolve_user(void *raw) -> std::optional<howdy::native::InvokingUser> {
		auto &context = *static_cast<TestContext *>(raw);
		record(context, 0);
		return context.invoking_user;
	}

	auto resolve_editor(void *raw, bool allow_env_editor) -> std::string {
		auto &context = *static_cast<TestContext *>(raw);
		record(context, 1);
		context.allow_env_editor = allow_env_editor;
		if (context.throw_from_resolve_editor) {
			throw std::runtime_error("test editor exception");
		}
		return context.editor;
	}

	auto resolve_path(void *raw) -> std::filesystem::path {
		auto &context = *static_cast<TestContext *>(raw);
		record(context, 2);
		return context.config_path;
	}

	auto check_security(void *raw, const std::filesystem::path &path)
	    -> howdy::native::ConfigPathCheckResult {
		auto &context = *static_cast<TestContext *>(raw);
		record(context, 3);
		if (path != context.config_path) {
			context.security = {.ok = false, .error_message = "wrong security path"};
		}
		return context.security;
	}

	auto create_temp(void *raw, const std::filesystem::path &path,
	                 const std::optional<howdy::native::InvokingUser> &user)
	    -> std::optional<TempConfigCopy> {
		auto &context = *static_cast<TestContext *>(raw);
		record(context, 4);
		if (path != context.config_path || user.has_value() != context.invoking_user.has_value()) {
			return std::nullopt;
		}
		return context.temp_copy;
	}

	auto run_editor(void *raw, const std::string &editor, const std::filesystem::path &path,
	                const std::optional<howdy::native::InvokingUser> &user) -> int {
		auto &context = *static_cast<TestContext *>(raw);
		record(context, 5);
		if (editor != context.editor || path != kTempPath ||
		    user.has_value() != context.invoking_user.has_value()) {
			return -1;
		}
		return context.editor_status;
	}

	auto read_snapshot(void *raw, const std::filesystem::path &path, std::string *content) -> bool {
		auto &context = *static_cast<TestContext *>(raw);
		record(context, 6);
		if (path != kTempPath || content == nullptr) {
			return false;
		}
		*content = context.edited_content;
		return context.read_result;
	}

	auto validate(void *raw, const std::string &content, std::string *error) -> bool {
		auto &context = *static_cast<TestContext *>(raw);
		record(context, 7);
		if (content != context.edited_content || error == nullptr) {
			return false;
		}
		*error = context.validation_error;
		return context.validation_result;
	}

	auto matches(void *raw, const std::filesystem::path &path, const std::string &content) -> bool {
		auto &context = *static_cast<TestContext *>(raw);
		record(context, 8);
		return path == context.config_path && content == context.edited_content &&
		       context.content_matches;
	}

	auto install(void *raw, const std::filesystem::path &path, const std::string &content,
	             std::string *error, bool lock, bool validate_runtime, const std::string *expected)
	    -> bool {
		auto &context = *static_cast<TestContext *>(raw);
		record(context, 9);
		context.installer_path             = path;
		context.installer_content          = content;
		context.installer_lock             = lock;
		context.installer_validate_runtime = validate_runtime;
		context.installer_expected_nonnull = expected != nullptr;
		context.installer_expected_content = expected == nullptr ? "" : *expected;
		*error                             = context.installer_error;
		return context.installer_result;
	}

	void remove_path(void *raw, const std::filesystem::path &path) {
		auto &context = *static_cast<TestContext *>(raw);
		record(context, 10);
		context.removed_paths.push_back(path);
	}

	void editor_ready(void *raw, const std::string &editor) {
		auto &context = *static_cast<TestContext *>(raw);
		record(context, 11);
		++context.editor_ready_calls;
		context.editor_ready_editor = editor;
	}

	auto dependencies_for(TestContext &context) -> ConfigDependencies {
		return {
		    .context                           = &context,
		    .resolve_invoking_user             = resolve_user,
		    .resolve_editor                    = resolve_editor,
		    .resolve_config_path               = resolve_path,
		    .check_secure_config_path          = check_security,
		    .create_temp_copy                  = create_temp,
		    .run_editor                        = run_editor,
		    .read_temp_config_snapshot         = read_snapshot,
		    .validate_config_content           = validate,
		    .file_content_matches              = matches,
		    .replace_config_content_atomically = install,
		    .remove_if_exists                  = remove_path,
		};
	}

	void remove_dependency(ConfigDependencies &dependencies, std::size_t missing) {
		switch (missing) {
			case 0:
				dependencies.resolve_invoking_user = nullptr;
				break;
			case 1:
				dependencies.resolve_editor = nullptr;
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

	auto run_config(const ConfigDependencies &dependencies) -> RunResult {
		std::array<char *, 1> argv{const_cast<char *>("howdy-config")};
		std::ostringstream    output;
		ScopedStreamBuffer    stdout_guard(std::cout, output.rdbuf());
		const int exit_code = howdy::native::config_internal::config_main_with_dependencies(
		    1, argv.data(), dependencies);
		return {.exit_code = exit_code, .output = output.str()};
	}

	auto expect_removed_once(const TestContext &context, const std::string &message) -> bool {
		return expect(context.removed_paths == std::vector<std::filesystem::path>{kTempPath},
		              message);
	}

	auto expect_installer_arguments(const TestContext &context, const std::string &message)
	    -> bool {
		return expect(context.installer_path == kConfigPath &&
		                  context.installer_content == kEditedContent && context.installer_lock &&
		                  !context.installer_validate_runtime &&
		                  context.installer_expected_nonnull &&
		                  context.installer_expected_content == kOriginalContent,
		              message);
	}

	auto write_file(const std::filesystem::path &path, const std::string &content) -> bool {
		std::ofstream output(path);
		output << content;
		return output.good();
	}

	auto read_file(const std::filesystem::path &path) -> std::string {
		std::ifstream input(path);
		return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	}

	auto path_reported_after(const std::string &output, const std::string &prefix)
	    -> std::filesystem::path {
		const auto start = output.find(prefix);
		if (start == std::string::npos) {
			return {};
		}
		const auto path_start = start + prefix.size();
		const auto path_end   = output.find('\n', path_start);
		return output.substr(path_start, path_end - path_start);
	}

	auto public_entrypoint_preserves_invalid_edit() -> bool {
		namespace fs = std::filesystem;

		const std::string temp_template =
		    (fs::current_path() / "howdy-config-cli-integration-XXXXXX").string();
		std::vector<char> writable_template(temp_template.begin(), temp_template.end());
		writable_template.push_back('\0');
		const char *created_path = mkdtemp(writable_template.data());
		if (!expect(created_path != nullptr, "integration creates temp directory")) {
			return false;
		}

		bool            ok          = true;
		const fs::path  temp_root   = created_path;
		const fs::path  config_path = temp_root / "config.ini";
		const fs::path  editor_path = temp_root / "fake-editor";
		std::error_code error;
		ok &= expect(write_file(config_path, std::string{kOriginalContent}),
		             "integration writes config baseline");
		ok &= expect(chmod(config_path.c_str(), 0600) == 0, "integration secures config file");
		ok &= expect(write_file(editor_path, "#!/bin/sh\nprintf '[core\\n' > \"$1\"\n"),
		             "integration writes fake editor");
		ok &= expect(chmod(editor_path.c_str(), 0700) == 0,
		             "integration makes fake editor executable");

		ScopedEnvironmentVariable editor_env("EDITOR", editor_path.string());
		ScopedEnvironmentVariable config_env("HOWDY_CONFIG", config_path.string());
		ScopedEnvironmentVariable sudo_uid_env("SUDO_UID", std::to_string(getuid()));
		ScopedEnvironmentVariable sudo_gid_env("SUDO_GID", std::to_string(getgid()));

		std::array<char *, 1> argv{const_cast<char *>("howdy-config")};
		std::ostringstream    output;
		int                   exit_code = 0;
		{
			ScopedStreamBuffer stdout_guard(std::cout, output.rdbuf());
			exit_code = config_main(1, argv.data());
		}

		constexpr std::string_view recovery_prefix =
		    "Edited config is invalid and was not installed: ";
		const auto reported_temp_path =
		    path_reported_after(output.str(), std::string(recovery_prefix));
		ok &= expect(exit_code == 1, "integration aborts invalid edit");
		ok &= expect(output.str() == "Editing config.ini in fake-editor\n" +
		                                 std::string(recovery_prefix) +
		                                 reported_temp_path.string() + "\n",
		             "integration output exact");
		ok &= expect(!reported_temp_path.empty(), "integration reports recovery path");
		ok &= expect(fs::exists(reported_temp_path, error) && !error,
		             "integration preserves invalid temp file");
		ok &= expect(read_file(reported_temp_path) == "[core\n",
		             "integration keeps malformed content");
		fs::remove(reported_temp_path, error);
		fs::remove_all(temp_root, error);
		return ok;
	}

	auto production_config_reads_are_bounded() -> bool {
		namespace fs = std::filesystem;

		bool       ok = true;
		const auto dependencies =
		    howdy::native::config_internal::default_config_edit_dependencies();
		const fs::path    source_path = fs::current_path() / "howdy-config-cli-size-test.ini";
		const std::string at_limit(howdy::native::kMaxConfigFileSize, 'x');
		const std::string over_limit(howdy::native::kMaxConfigFileSize + 1, 'x');
		std::error_code   error;

		ok &= expect(write_file(source_path, at_limit), "write source config at size limit");
		const auto copy =
		    dependencies.create_temp_copy(dependencies.context, source_path, std::nullopt);
		ok &= expect(copy.has_value() && copy->original_content == at_limit,
		             "source config exactly at size limit is copied");
		if (copy.has_value()) {
			std::string snapshot;
			ok &= expect(dependencies.read_temp_config_snapshot(dependencies.context, copy->path,
			                                                    &snapshot) &&
			                 snapshot == at_limit,
			             "edited temp config exactly at size limit is read");
			ok &= expect(write_file(copy->path, over_limit), "write oversized edited temp config");
			ok &= expect(!dependencies.read_temp_config_snapshot(dependencies.context, copy->path,
			                                                     &snapshot),
			             "oversized edited temp config is rejected");
			dependencies.remove_if_exists(dependencies.context, copy->path);
		}

		ok &= expect(write_file(source_path, over_limit), "write oversized source config");
		ok &=
		    expect(!dependencies.create_temp_copy(dependencies.context, source_path, std::nullopt),
		           "oversized source config is rejected before temp copy");
		fs::remove(source_path, error);
		return ok;
	}

	auto stdout_restores_after_callback_exception() -> bool {
		bool               ok = true;
		std::ostringstream restored_output;
		ScopedStreamBuffer outer_stdout_guard(std::cout, restored_output.rdbuf());
		auto              *original_stdout = std::cout.rdbuf();
		TestContext        context;
		context.throw_from_resolve_editor = true;
		bool exception_observed           = false;
		try {
			(void)run_config(dependencies_for(context));
		} catch (const std::runtime_error &) {
			exception_observed = true;
		}
		ok &= expect(exception_observed, "editor callback exception escapes test runner");
		ok &= expect(std::cout.rdbuf() == original_stdout,
		             "stdout buffer restores after callback exception");
		std::cout << "stdout-restored-marker";
		ok &= expect(restored_output.str() == "stdout-restored-marker",
		             "stdout marker reaches restored buffer");
		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	ok &= stdout_restores_after_callback_exception();

	ok &= public_entrypoint_preserves_invalid_edit();
	ok &= production_config_reads_are_bounded();

	{
		TestContext             context;
		const ConfigEditSession session(dependencies_for(context));
		const auto result = session.run({.context = &context, .editor_ready = editor_ready});
		ok &= expect(result.status == ConfigEditStatus::kOk,
		             "temporary dependencies session succeeds");
		ok &= expect(result.error.empty() && result.temp_path == kTempPath &&
		                 result.editor == context.editor,
		             "temporary dependencies result matches success path");
		ok &= expect(context.order == std::vector{0, 1, 2, 3, 4, 11, 5, 6, 7, 8, 9, 10},
		             "temporary dependencies invokes callbacks once in order");
		ok &=
		    expect(context.editor_ready_calls == 1 && context.editor_ready_editor == context.editor,
		           "temporary dependencies invokes ready callback");
		ok &= expect_removed_once(context, "temporary dependencies removes temp once");
		ok &=
		    expect_installer_arguments(context, "temporary dependencies installer arguments exact");
	}

	{
		TestContext             context;
		const ConfigEditSession session(ConfigDependencies{});
		const auto result = session.run({.context = &context, .editor_ready = editor_ready});
		ok &= expect(result.status == ConfigEditStatus::kDependenciesUnavailable,
		             "empty dependencies session reports unavailable dependencies");
		ok &= expect(context.order.empty(), "empty dependencies session invokes no callbacks");
	}

	for (std::size_t missing = 0; missing < 11; ++missing) {
		TestContext context;
		auto        dependencies = dependencies_for(context);
		remove_dependency(dependencies, missing);

		const ConfigEditSession session(dependencies);
		const auto result = session.run({.context = &context, .editor_ready = editor_ready});
		ok &= expect(result.status == ConfigEditStatus::kDependenciesUnavailable,
		             "incomplete dependencies session reports unavailable dependencies");
		ok &= expect(result.error.empty() && result.temp_path.empty() && result.editor.empty(),
		             "incomplete dependencies result has no side effects");
		ok &= expect(context.order.empty(), "incomplete dependencies session invokes no callbacks");
		ok &= expect(context.editor_ready_calls == 0,
		             "incomplete dependencies session skips ready callback");
	}

	for (std::size_t missing = 0; missing < 11; ++missing) {
		TestContext context;
		auto        dependencies = dependencies_for(context);
		remove_dependency(dependencies, missing);
		const auto result = run_config(dependencies);
		ok &= expect(result.exit_code == 1 && result.output.empty(),
		             "null dependency aborts silently");
		ok &= expect(context.order.empty(), "null dependency invokes no callbacks");
	}

	{
		TestContext context;
		context.editor    = {};
		const auto result = run_config(dependencies_for(context));
		ok &= expect(result.exit_code == 1 && result.output ==
		                                          "Error: Could not find a suitable text editor.\n"
		                                          "Set EDITOR to an absolute executable path, or "
		                                          "install one of: micro, nano, vi.\n",
		             "no editor output exact");
		ok &= expect(context.order == std::vector{0, 1}, "no editor stops before config path");
	}

	for (const bool user_exists : {true, false}) {
		TestContext context;
		if (!user_exists) {
			context.invoking_user = std::nullopt;
		}
		run_config(dependencies_for(context));
		ok &= expect(context.allow_env_editor == user_exists,
		             "editor env permission follows invoking user");
	}

	{
		TestContext context;
		context.security  = {.ok = false, .error_message = "Config path is insecure"};
		const auto result = run_config(dependencies_for(context));
		ok &= expect(result.exit_code == 1 && result.output == "Config path is insecure\n",
		             "unsafe path output exact");
		ok &=
		    expect(context.order == std::vector{0, 1, 2, 3}, "unsafe path stops before temp copy");
	}

	{
		TestContext context;
		context.temp_copy = std::nullopt;
		const auto result = run_config(dependencies_for(context));
		ok &= expect(result.exit_code == 1 &&
		                 result.output == "Failed to prepare a temporary config copy\n",
		             "temp-copy failure output exact");
		ok &=
		    expect(context.order == std::vector{0, 1, 2, 3, 4}, "temp failure stops before editor");
	}

	{
		TestContext context;
		context.editor_status = -1;
		const auto result     = run_config(dependencies_for(context));
		ok &= expect(result.exit_code == 1 &&
		                 result.output == "Editing config.ini in nano\nFailed to launch editor\n",
		             "editor launch failure output exact");
		ok &= expect_removed_once(context, "launch failure removes temp once");
		ok &= expect(context.order == std::vector{0, 1, 2, 3, 4, 5, 10},
		             "launch failure stops after cleanup");
	}

	for (const int status : {W_EXITCODE(2, 0), 15}) {
		TestContext context;
		context.editor_status = status;
		const auto result     = run_config(dependencies_for(context));
		ok &= expect(result.exit_code == 1 &&
		                 result.output == "Editing config.ini in nano\n"
		                                  "Editor exited unsuccessfully; config not updated\n",
		             "unsuccessful editor output exact");
		ok &= expect_removed_once(context, "unsuccessful editor removes temp once");
		ok &= expect(context.calls[6] == 0, "unsuccessful editor skips later callbacks");
	}

	{
		TestContext context;
		context.read_result = false;
		const auto result   = run_config(dependencies_for(context));
		ok &= expect(result.exit_code == 1 &&
		                 result.output ==
		                     "Editing config.ini in nano\nFailed to install edited config\n",
		             "snapshot failure output exact");
		ok &= expect_removed_once(context, "snapshot failure removes temp once");
		ok &= expect(context.calls[7] == 0 && context.calls[8] == 0 && context.calls[9] == 0,
		             "snapshot failure skips later callbacks");
	}

	for (const auto &[error, expected] : std::array<std::pair<std::string, std::string>, 3>{
	         std::pair{"Updated config is invalid",
	                   "Editing config.ini in nano\nEdited config is invalid and was not "
	                   "installed: /tmp/howdy-config-test\n"},
	         std::pair{"Invalid timeout", "Editing config.ini in nano\nInvalid timeout\n"},
	         std::pair{"", "Editing config.ini in nano\nFailed to install edited config\n"}}) {
		TestContext context;
		context.validation_result = false;
		context.validation_error  = error;
		const auto result         = run_config(dependencies_for(context));
		ok &=
		    expect(result.exit_code == 1 && result.output == expected, "invalid edit output exact");
		ok &= expect(context.removed_paths.empty(), "invalid edit preserves temp");
		ok &= expect(context.calls[8] == 0 && context.calls[9] == 0, "invalid edit skips install");
	}

	{
		TestContext context;
		context.content_matches = true;
		const auto result       = run_config(dependencies_for(context));
		ok &= expect(result.exit_code == 0 &&
		                 result.output == "Editing config.ini in nano\nNo config changes made\n",
		             "no-op output exact");
		ok &= expect_removed_once(context, "no-op removes temp once");
		ok &= expect(context.calls[9] == 0, "no-op skips installer");
	}

	for (const auto &[error, expected] : std::array<std::pair<std::string, std::string>, 2>{
	         std::pair{"Config changed while editor was open",
	                   "Editing config.ini in nano\nConfig changed while editor was open\n"},
	         std::pair{"", "Editing config.ini in nano\nFailed to install edited config\n"}}) {
		TestContext context;
		context.installer_result = false;
		context.installer_error  = error;
		const auto result        = run_config(dependencies_for(context));
		ok &= expect(result.exit_code == 1 && result.output == expected,
		             "install failure output exact");
		ok &= expect_removed_once(context, "install failure removes temp once");
		ok &= expect_installer_arguments(context, "install failure arguments exact");
	}

	{
		TestContext context;
		const auto  result = run_config(dependencies_for(context));
		ok &= expect(result.exit_code == 0 &&
		                 result.output == "Editing config.ini in nano\nConfig updated\n",
		             "success output exact");
		ok &= expect(context.order == std::vector{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10},
		             "success invokes callbacks once in order");
		ok &= expect_removed_once(context, "success removes temp once");
		ok &= expect_installer_arguments(context, "success installer arguments exact");
	}

	return ok ? 0 : 1;
}
