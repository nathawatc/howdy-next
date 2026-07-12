#include "auth_flow_testing.hpp"
#include "runtime_session.hpp"
#include "runtime_session_testing.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <paths.hpp>
#include <spawn.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

#include <sys/wait.h>

namespace {

	constexpr auto kConfiguredConfig = "/etc/howdy/config.ini";
	constexpr auto kConfiguredModels = "/var/lib/howdy/models";
	constexpr auto kStagedRoot       = "/run/howdy/pam-test";
	constexpr auto kStagedConfig     = "/run/howdy/pam-test/config.ini";
	constexpr auto kStagedModels     = "/run/howdy/pam-test/models";

	struct FakeContext {
		std::vector<howdy::native::RuntimeConfigLoadResult> load_results;
		std::vector<std::filesystem::path>                  load_paths;
		std::vector<std::string>                            prepare_usernames;
		std::vector<std::filesystem::path>                  cleanup_roots;
		int                                                 effective_uid_calls = 0;
		bool                                                prepare_succeeds    = true;
		uid_t                                               effective_uid       = 0;
		howdy::pam::PreparedRuntimeFiles                    prepared{
		    .root_dir        = kStagedRoot,
		    .config_path     = kStagedConfig,
		    .user_models_dir = kStagedModels,
		};
	};

	struct CallbackCounts {
		std::size_t load          = 0;
		std::size_t prepare       = 0;
		std::size_t cleanup       = 0;
		int         effective_uid = 0;

		auto operator==(const CallbackCounts &) const -> bool = default;
	};

	auto expect(bool condition, const std::string &message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << "\n";
			return false;
		}
		return true;
	}

	auto success_result(const std::filesystem::path &path)
	    -> howdy::native::RuntimeConfigLoadResult {
		return howdy::native::RuntimeConfigLoadResult{
		    .ok     = true,
		    .status = howdy::native::RuntimeConfigLoadStatus::kOk,
		    .path   = path,
		    .config = howdy::native::RuntimeConfig{},
		};
	}

	auto path_error_result(int error_code) -> howdy::native::RuntimeConfigLoadResult {
		return howdy::native::RuntimeConfigLoadResult{
		    .status        = howdy::native::RuntimeConfigLoadStatus::kPathError,
		    .path          = kConfiguredConfig,
		    .error_message = "path error",
		    .error_code    = error_code,
		};
	}

	auto parse_error_result(const std::filesystem::path &path)
	    -> howdy::native::RuntimeConfigLoadResult {
		return howdy::native::RuntimeConfigLoadResult{
		    .status        = howdy::native::RuntimeConfigLoadStatus::kParseError,
		    .path          = path,
		    .error_message = "parse error",
		};
	}

	auto prepare_runtime(void *context, std::string_view username,
	                     howdy::pam::PreparedRuntimeFiles *prepared) -> bool {
		auto &fake = *static_cast<FakeContext *>(context);
		fake.prepare_usernames.emplace_back(username);
		if (!fake.prepare_succeeds) {
			return false;
		}
		*prepared = fake.prepared;
		return true;
	}

	auto cleanup_runtime(void *context, const std::filesystem::path &root_dir) -> void {
		auto &fake = *static_cast<FakeContext *>(context);
		fake.cleanup_roots.push_back(root_dir);
	}

	auto load_runtime_config(void *context, const std::filesystem::path &config_path)
	    -> howdy::native::RuntimeConfigLoadResult {
		auto &fake = *static_cast<FakeContext *>(context);
		fake.load_paths.push_back(config_path);
		const auto result_index = fake.load_paths.size() - 1;
		if (result_index >= fake.load_results.size()) {
			return parse_error_result(config_path);
		}
		return fake.load_results[result_index];
	}

	auto effective_uid(void *context) -> uid_t {
		auto &fake = *static_cast<FakeContext *>(context);
		++fake.effective_uid_calls;
		return fake.effective_uid;
	}

	auto dependencies(FakeContext *context) -> howdy::pam::RuntimeSessionDependencies {
		return howdy::pam::RuntimeSessionDependencies{
		    .context             = context,
		    .prepare_runtime     = prepare_runtime,
		    .cleanup_runtime     = cleanup_runtime,
		    .load_runtime_config = load_runtime_config,
		    .effective_uid       = effective_uid,
		};
	}

	auto callback_counts(const FakeContext &context) -> CallbackCounts {
		return CallbackCounts{
		    .load          = context.load_paths.size(),
		    .prepare       = context.prepare_usernames.size(),
		    .cleanup       = context.cleanup_roots.size(),
		    .effective_uid = context.effective_uid_calls,
		};
	}

	auto no_callbacks_ran(const FakeContext &context) -> bool {
		return context.load_paths.empty() && context.prepare_usernames.empty() &&
		       context.cleanup_roots.empty() && context.effective_uid_calls == 0;
	}

	auto test_direct_success() -> bool {
		FakeContext context{.load_results = {success_result(kConfiguredConfig)}};
		bool        ok = true;
		{
			howdy::pam::RuntimeSession session(kConfiguredConfig, kConfiguredModels,
			                                   dependencies(&context));
			const auto                 result = session.load_for_user("alice");
			ok &= expect(result.ok(), "direct success returns ok");
			ok &=
			    expect(context.load_paths == std::vector<std::filesystem::path>{kConfiguredConfig},
			           "direct success loads configured path once");
			ok &= expect(context.prepare_usernames.empty(), "direct success does not prepare");
			ok &= expect(!session.staged(), "direct success is not staged");
			ok &= expect(session.config_path() == kConfiguredConfig,
			             "direct success retains configured config path");
			ok &= expect(session.user_models_dir() == kConfiguredModels,
			             "direct success retains configured models path");
		}
		ok &= expect(context.cleanup_roots.empty(), "direct success does not clean up");
		return ok;
	}

	auto test_direct_parse_failure() -> bool {
		FakeContext context{.load_results = {parse_error_result(kConfiguredConfig)}};
		{
			howdy::pam::RuntimeSession session(kConfiguredConfig, kConfiguredModels,
			                                   dependencies(&context));
			const auto                 result = session.load_for_user("alice");
			if (!expect(result.status == howdy::pam::RuntimeSessionLoadStatus::kConfigLoadFailed,
			            "direct parse failure returns config-load failure") ||
			    !expect(context.prepare_usernames.empty(),
			            "direct parse failure does not prepare")) {
				return false;
			}
		}
		return expect(context.cleanup_roots.empty(), "direct parse failure does not clean up");
	}

	auto test_root_eacces_failure() -> bool {
		FakeContext context{
		    .load_results  = {path_error_result(EACCES)},
		    .effective_uid = 0,
		};
		howdy::pam::RuntimeSession session(kConfiguredConfig, kConfiguredModels,
		                                   dependencies(&context));
		const auto                 result = session.load_for_user("alice");
		return expect(result.status == howdy::pam::RuntimeSessionLoadStatus::kConfigLoadFailed,
		              "root EACCES returns config-load failure") &&
		       expect(context.effective_uid_calls == 1, "root EACCES checks effective UID") &&
		       expect(context.prepare_usernames.empty(), "root EACCES does not prepare");
	}

	auto test_non_root_non_eacces_failure() -> bool {
		FakeContext context{
		    .load_results  = {path_error_result(ENOENT)},
		    .effective_uid = 1000,
		};
		howdy::pam::RuntimeSession session(kConfiguredConfig, kConfiguredModels,
		                                   dependencies(&context));
		const auto                 result = session.load_for_user("alice");
		return expect(result.status == howdy::pam::RuntimeSessionLoadStatus::kConfigLoadFailed,
		              "non-root non-EACCES returns config-load failure") &&
		       expect(context.effective_uid_calls == 0, "non-EACCES does not need effective UID") &&
		       expect(context.prepare_usernames.empty(), "non-root non-EACCES does not prepare");
	}

	auto test_staged_success() -> bool {
		FakeContext context{
		    .load_results  = {path_error_result(EACCES), success_result(kStagedConfig)},
		    .effective_uid = 1000,
		};
		bool ok = true;
		{
			howdy::pam::RuntimeSession session(kConfiguredConfig, kConfiguredModels,
			                                   dependencies(&context));
			const auto                 result = session.load_for_user("alice");
			ok &= expect(result.ok(), "staged config success returns ok");
			ok &= expect(context.load_paths ==
			                 std::vector<std::filesystem::path>{kConfiguredConfig, kStagedConfig},
			             "staged success loads configured then staged path");
			ok &= expect(context.prepare_usernames == std::vector<std::string>{"alice"},
			             "staged success prepares once for alice");
			ok &= expect(session.staged(), "staged success reports staged");
			ok &=
			    expect(session.config_path() == kStagedConfig, "staged success uses staged config");
			ok &= expect(session.user_models_dir() == kStagedModels,
			             "staged success uses staged models");
			ok &= expect(context.cleanup_roots.empty(), "staged success defers cleanup");
		}
		ok &= expect(context.cleanup_roots == std::vector<std::filesystem::path>{kStagedRoot},
		             "staged success cleans up once at destruction");
		return ok;
	}

	auto test_prepare_failure() -> bool {
		FakeContext context{
		    .load_results     = {path_error_result(EACCES)},
		    .prepare_succeeds = false,
		    .effective_uid    = 1000,
		};
		{
			howdy::pam::RuntimeSession session(kConfiguredConfig, kConfiguredModels,
			                                   dependencies(&context));
			const auto                 result = session.load_for_user("alice");
			if (!expect(result.status == howdy::pam::RuntimeSessionLoadStatus::kPrepareFailed,
			            "prepare failure returns prepare-failed") ||
			    !expect(context.load_paths.size() == 1,
			            "prepare failure does not load staged config") ||
			    !expect(context.prepare_usernames == std::vector<std::string>{"alice"},
			            "prepare failure calls prepare once")) {
				return false;
			}
		}
		return expect(context.cleanup_roots.empty(), "prepare failure does not clean up");
	}

	auto test_staged_config_failure() -> bool {
		FakeContext context{
		    .load_results  = {path_error_result(EACCES), parse_error_result(kStagedConfig)},
		    .effective_uid = 1000,
		};
		bool ok = true;
		{
			howdy::pam::RuntimeSession session(kConfiguredConfig, kConfiguredModels,
			                                   dependencies(&context));
			const auto                 result = session.load_for_user("alice");
			ok &= expect(result.status == howdy::pam::RuntimeSessionLoadStatus::kConfigLoadFailed,
			             "staged parse failure returns config-load failure");
			ok &= expect(context.cleanup_roots.empty(), "staged parse failure defers cleanup");
		}
		ok &= expect(context.cleanup_roots == std::vector<std::filesystem::path>{kStagedRoot},
		             "staged parse failure cleans up once at destruction");
		return ok;
	}

	auto test_missing_dependencies() -> bool {
		bool ok = true;
		for (int missing = 0; missing < 4; ++missing) {
			FakeContext context;
			auto        deps = dependencies(&context);
			switch (missing) {
				case 0:
					deps.prepare_runtime = nullptr;
					break;
				case 1:
					deps.cleanup_runtime = nullptr;
					break;
				case 2:
					deps.load_runtime_config = nullptr;
					break;
				case 3:
					deps.effective_uid = nullptr;
					break;
				default:
					break;
			}

			{
				howdy::pam::RuntimeSession session(kConfiguredConfig, kConfiguredModels, deps);
				const auto                 result = session.load_for_user("alice");
				ok &= expect(result.status ==
				                 howdy::pam::RuntimeSessionLoadStatus::kInvalidDependencies,
				             "missing dependency returns invalid-dependencies");
			}
			ok &= expect(no_callbacks_ran(context), "missing dependency invokes no callbacks");
		}
		return ok;
	}

	auto test_no_duplicate_cleanup() -> bool {
		FakeContext context{
		    .load_results  = {path_error_result(EACCES), success_result(kStagedConfig)},
		    .effective_uid = 1000,
		};
		{
			howdy::pam::RuntimeSession session(kConfiguredConfig, kConfiguredModels,
			                                   dependencies(&context));
			if (!expect(session.load_for_user("alice").ok(),
			            "duplicate-cleanup setup stages successfully")) {
				return false;
			}
		}
		return expect(context.cleanup_roots.size() == 1, "scope exit invokes cleanup exactly once");
	}

	auto test_direct_success_is_one_shot() -> bool {
		FakeContext context{.load_results = {success_result(kConfiguredConfig)}};
		bool        ok = true;
		{
			howdy::pam::RuntimeSession session(kConfiguredConfig, kConfiguredModels,
			                                   dependencies(&context));
			const auto                 first = session.load_for_user("alice");
			ok &= expect(first.status == howdy::pam::RuntimeSessionLoadStatus::kOk,
			             "direct one-shot first load succeeds");
			const auto counts_after_first = callback_counts(context);

			const auto second = session.load_for_user("bob");
			ok &= expect(second.status == howdy::pam::RuntimeSessionLoadStatus::kAlreadyLoaded,
			             "direct one-shot rejects second load");
			ok &= expect(!second.ok(), "already-loaded direct result is not ok");
			ok &= expect(callback_counts(context) == counts_after_first,
			             "direct re-entry invokes no callbacks");
			ok &= expect(session.config_path() == kConfiguredConfig,
			             "direct re-entry preserves configured config path");
			ok &= expect(session.user_models_dir() == kConfiguredModels,
			             "direct re-entry preserves configured models path");
			ok &= expect(context.cleanup_roots.empty(), "direct re-entry does not clean up early");
		}
		ok &= expect(context.cleanup_roots.empty(),
		             "direct re-entry does not clean up at destruction");
		return ok;
	}

	auto test_staged_success_is_one_shot() -> bool {
		constexpr auto kRuntimeRoot   = "/run/howdy/runtime-a";
		constexpr auto kRuntimeConfig = "/run/howdy/runtime-a/config.ini";
		constexpr auto kRuntimeModels = "/run/howdy/runtime-a/models";
		FakeContext    context{
		    .load_results  = {path_error_result(EACCES), success_result(kRuntimeConfig)},
		    .effective_uid = 1000,
		    .prepared{
		        .root_dir        = kRuntimeRoot,
		        .config_path     = kRuntimeConfig,
		        .user_models_dir = kRuntimeModels,
		    },
		};
		bool ok = true;
		{
			howdy::pam::RuntimeSession session(kConfiguredConfig, kConfiguredModels,
			                                   dependencies(&context));
			const auto                 first = session.load_for_user("alice");
			ok &= expect(first.status == howdy::pam::RuntimeSessionLoadStatus::kOk,
			             "staged one-shot first load succeeds");
			ok &= expect(session.config_path() == kRuntimeConfig,
			             "staged one-shot activates runtime-a config");
			ok &= expect(session.user_models_dir() == kRuntimeModels,
			             "staged one-shot activates runtime-a models");
			const auto counts_after_first = callback_counts(context);

			const auto second = session.load_for_user("bob");
			ok &= expect(second.status == howdy::pam::RuntimeSessionLoadStatus::kAlreadyLoaded,
			             "staged one-shot rejects second load");
			ok &= expect(callback_counts(context) == counts_after_first,
			             "staged re-entry invokes no callbacks");
			ok &= expect(session.config_path() == kRuntimeConfig,
			             "staged re-entry preserves runtime-a config");
			ok &= expect(session.user_models_dir() == kRuntimeModels,
			             "staged re-entry preserves runtime-a models");
		}
		ok &= expect(context.cleanup_roots == std::vector<std::filesystem::path>{kRuntimeRoot},
		             "staged re-entry cleans runtime-a exactly once");
		return ok;
	}

	auto test_failed_staged_load_is_one_shot() -> bool {
		constexpr auto kRuntimeRoot   = "/run/howdy/runtime-failed";
		constexpr auto kRuntimeConfig = "/run/howdy/runtime-failed/config.ini";
		constexpr auto kRuntimeModels = "/run/howdy/runtime-failed/models";
		FakeContext    context{
		    .load_results  = {path_error_result(EACCES), parse_error_result(kRuntimeConfig)},
		    .effective_uid = 1000,
		    .prepared{
		        .root_dir        = kRuntimeRoot,
		        .config_path     = kRuntimeConfig,
		        .user_models_dir = kRuntimeModels,
		    },
		};
		bool ok = true;
		{
			howdy::pam::RuntimeSession session(kConfiguredConfig, kConfiguredModels,
			                                   dependencies(&context));
			const auto                 first = session.load_for_user("alice");
			ok &= expect(first.status == howdy::pam::RuntimeSessionLoadStatus::kConfigLoadFailed,
			             "failed staged one-shot reports config failure");
			const auto counts_after_first = callback_counts(context);

			const auto second = session.load_for_user("bob");
			ok &= expect(second.status == howdy::pam::RuntimeSessionLoadStatus::kAlreadyLoaded,
			             "failed staged one-shot rejects second load");
			ok &= expect(callback_counts(context) == counts_after_first,
			             "failed staged re-entry invokes no callbacks");
		}
		ok &= expect(context.cleanup_roots == std::vector<std::filesystem::path>{kRuntimeRoot},
		             "failed staged re-entry cleans original root exactly once");
		return ok;
	}

	auto test_invalid_dependencies_are_one_shot() -> bool {
		FakeContext context;
		auto        deps     = dependencies(&context);
		deps.prepare_runtime = nullptr;
		bool ok              = true;
		{
			howdy::pam::RuntimeSession session(kConfiguredConfig, kConfiguredModels, deps);
			const auto                 first = session.load_for_user("alice");
			ok &= expect(first.status == howdy::pam::RuntimeSessionLoadStatus::kInvalidDependencies,
			             "invalid dependency first load fails validation");
			ok &= expect(no_callbacks_ran(context),
			             "invalid dependency first load invokes no callbacks");

			const auto second = session.load_for_user("bob");
			ok &= expect(second.status == howdy::pam::RuntimeSessionLoadStatus::kAlreadyLoaded,
			             "invalid dependency session rejects second load");
			ok &= expect(no_callbacks_ran(context),
			             "invalid dependency re-entry invokes no callbacks");
		}
		ok &= expect(context.cleanup_roots.empty(), "invalid dependency re-entry never cleans up");
		return ok;
	}

	struct AuthHelperSpawnFake {
		std::vector<std::string>         operations;
		std::vector<std::array<int, 2>>  pipe_fds;
		std::vector<int>                 pipe_flags;
		std::vector<std::pair<int, int>> duplicate_fd_requests;
		std::vector<std::pair<int, int>> dup2_fds;
		std::vector<int>                 action_close_fds;
		std::vector<int>                 parent_close_fds;
		std::vector<std::string>         spawn_argv;
		std::vector<std::string>         spawn_env;
		std::vector<std::string>         log_messages;
		std::string                      spawn_path;
		std::array<int, 2>               next_pipe_fds          = {10, 11};
		int                              next_duplicate_fd      = 20;
		int                              actions_init_result    = 0;
		int                              stdout_dup_result      = 0;
		int                              stderr_dup_result      = 0;
		int                              first_close_result     = 0;
		int                              final_close_result     = 0;
		int                              actions_destroy_result = 0;
		int                              spawn_result           = 0;
		int                              actions_init_calls     = 0;
		int                              actions_destroy_calls  = 0;
		int                              spawn_calls            = 0;
		int                              output_reader_calls    = 0;
		pid_t                            spawned_pid            = -1;
	};

	AuthHelperSpawnFake *g_auth_helper_spawn_fake = nullptr;

	auto fake_pipe2(void *context, int *pipe_fds, int flags) -> int {
		auto &fake = *static_cast<AuthHelperSpawnFake *>(context);
		fake.operations.emplace_back("pipe2");
		fake.pipe_fds.push_back(fake.next_pipe_fds);
		fake.pipe_flags.push_back(flags);
		pipe_fds[0] = fake.next_pipe_fds[0];
		pipe_fds[1] = fake.next_pipe_fds[1];
		return 0;
	}

	auto fake_duplicate_fd(void *context, int fd, int minimum_fd) -> int {
		auto &fake = *static_cast<AuthHelperSpawnFake *>(context);
		fake.operations.emplace_back("duplicate_fd");
		fake.duplicate_fd_requests.emplace_back(fd, minimum_fd);
		return fake.next_duplicate_fd++;
	}

	auto fake_actions_init(void *context, posix_spawn_file_actions_t *actions) -> int {
		(void)actions;
		auto &fake = *static_cast<AuthHelperSpawnFake *>(context);
		fake.operations.emplace_back("actions_init");
		++fake.actions_init_calls;
		return fake.actions_init_result;
	}

	auto fake_actions_adddup2(void *context, posix_spawn_file_actions_t *actions, int old_fd,
	                          int new_fd) -> int {
		(void)actions;
		auto &fake = *static_cast<AuthHelperSpawnFake *>(context);
		fake.operations.emplace_back(new_fd == STDOUT_FILENO ? "actions_adddup2_stdout"
		                                                     : "actions_adddup2_stderr");
		fake.dup2_fds.emplace_back(old_fd, new_fd);
		return new_fd == STDOUT_FILENO ? fake.stdout_dup_result : fake.stderr_dup_result;
	}

	auto fake_actions_addclose(void *context, posix_spawn_file_actions_t *actions, int fd) -> int {
		(void)actions;
		auto &fake = *static_cast<AuthHelperSpawnFake *>(context);
		fake.operations.emplace_back(fake.action_close_fds.empty() ? "actions_addclose_read"
		                                                           : "actions_addclose_write");
		fake.action_close_fds.push_back(fd);
		return fake.action_close_fds.size() == 1 ? fake.first_close_result
		                                         : fake.final_close_result;
	}

	auto fake_actions_destroy(void *context, posix_spawn_file_actions_t *actions) -> int {
		(void)actions;
		auto &fake = *static_cast<AuthHelperSpawnFake *>(context);
		fake.operations.emplace_back("actions_destroy");
		++fake.actions_destroy_calls;
		return fake.actions_destroy_result;
	}

	auto fake_spawn(void *context, pid_t *child_pid, const char *path,
	                const posix_spawn_file_actions_t *actions, char *const *argv, char *const *env)
	    -> int {
		(void)actions;
		auto &fake = *static_cast<AuthHelperSpawnFake *>(context);
		fake.operations.emplace_back("spawn");
		++fake.spawn_calls;
		fake.spawn_path = path;
		for (auto *const *argument = argv; argument != nullptr && *argument != nullptr;
		     ++argument) {
			fake.spawn_argv.emplace_back(*argument);
		}
		for (auto *const *environment = env; environment != nullptr && *environment != nullptr;
		     ++environment) {
			fake.spawn_env.emplace_back(*environment);
		}
		if (fake.spawn_result != 0) {
			return fake.spawn_result;
		}

		const pid_t pid = fork();
		if (pid < 0) {
			return errno;
		}
		if (pid == 0) {
			_exit(EXIT_SUCCESS);
		}
		*child_pid       = pid;
		fake.spawned_pid = pid;
		return 0;
	}

	auto fake_close(void *context, int fd) -> int {
		auto &fake = *static_cast<AuthHelperSpawnFake *>(context);
		fake.operations.emplace_back("close");
		fake.parent_close_fds.push_back(fd);
		return 0;
	}

	auto fake_auth_helper_output_reader([[maybe_unused]] int         fd,
	                                    [[maybe_unused]] std::size_t max_bytes)
	    -> howdy::native::BoundedReadResult {
		if (g_auth_helper_spawn_fake != nullptr) {
			g_auth_helper_spawn_fake->operations.emplace_back("read_output");
			++g_auth_helper_spawn_fake->output_reader_calls;
		}
		return {
		    .output = "CONFIG_PATH=/run/howdy/auth-helper/config.ini\n"
		              "USER_MODELS_DIR=/run/howdy/auth-helper/models\n",
		};
	}

	auto fake_auth_helper_spawn_log(std::string_view message) -> void {
		if (g_auth_helper_spawn_fake != nullptr) {
			g_auth_helper_spawn_fake->log_messages.emplace_back(message);
		}
	}

	class ScopedAuthHelperSpawnHooks {
	public:
		explicit ScopedAuthHelperSpawnHooks(AuthHelperSpawnFake *fake)
		    : previous_log_fn_(
		          howdy::pam::testing::set_auth_helper_spawn_log_fn(fake_auth_helper_spawn_log))
		    , previous_output_reader_(howdy::pam::testing::set_auth_helper_output_reader(
		          fake_auth_helper_output_reader)) {
			g_auth_helper_spawn_fake = fake;
		}

		ScopedAuthHelperSpawnHooks(const ScopedAuthHelperSpawnHooks &)                     = delete;
		auto operator=(const ScopedAuthHelperSpawnHooks &) -> ScopedAuthHelperSpawnHooks & = delete;

		~ScopedAuthHelperSpawnHooks() {
			howdy::pam::testing::set_auth_helper_output_reader(previous_output_reader_);
			howdy::pam::testing::set_auth_helper_spawn_log_fn(previous_log_fn_);
			g_auth_helper_spawn_fake = nullptr;
		}

	private:
		howdy::pam::testing::AuthHelperSpawnLogFn   previous_log_fn_        = nullptr;
		howdy::pam::testing::AuthHelperOutputReader previous_output_reader_ = nullptr;
	};

	auto spawn_operations(AuthHelperSpawnFake *fake)
	    -> howdy::pam::testing::AuthHelperSpawnOperations {
		return {
		    .context             = fake,
		    .pipe2_fn            = fake_pipe2,
		    .duplicate_fd_fn     = fake_duplicate_fd,
		    .actions_init_fn     = fake_actions_init,
		    .actions_adddup2_fn  = fake_actions_adddup2,
		    .actions_addclose_fn = fake_actions_addclose,
		    .actions_destroy_fn  = fake_actions_destroy,
		    .spawn_fn            = fake_spawn,
		    .close_fn            = fake_close,
		};
	}

	auto expect_parent_pipe_closed_once(const AuthHelperSpawnFake &fake, std::string_view name)
	    -> bool {
		return expect(std::count(fake.parent_close_fds.begin(), fake.parent_close_fds.end(),
		                         fake.next_pipe_fds[0]) == 1,
		              std::string(name) + " closes parent read fd once") &&
		       expect(std::count(fake.parent_close_fds.begin(), fake.parent_close_fds.end(),
		                         fake.next_pipe_fds[1]) == 1,
		              std::string(name) + " closes parent write fd once");
	}

	auto action_operations(const AuthHelperSpawnFake &fake) -> std::vector<std::string> {
		std::vector<std::string> actions;
		for (const auto &operation : fake.operations) {
			if (operation.starts_with("actions_")) {
				actions.push_back(operation);
			}
		}
		return actions;
	}

	auto expect_primary_spawn_error(const AuthHelperSpawnFake &fake, std::string_view operation,
	                                int error_code, std::string_view name) -> bool {
		return expect(!fake.log_messages.empty(), std::string(name) + " logs primary error") &&
		       expect(fake.log_messages.front().contains(operation),
		              std::string(name) + " first log names failed operation") &&
		       expect(fake.log_messages.front().contains(std::strerror(error_code)),
		              std::string(name) + " first log includes strerror") &&
		       expect(fake.log_messages.front().contains(std::to_string(error_code)),
		              std::string(name) + " first log includes numeric error");
	}

	auto expect_destroy_error_after_primary(const AuthHelperSpawnFake &fake, std::string_view name)
	    -> bool {
		return expect(fake.log_messages.size() >= 2,
		              std::string(name) + " logs destroy error after primary error") &&
		       expect(fake.log_messages[1].contains("posix_spawn_file_actions_destroy"),
		              std::string(name) + " second log names action destroy") &&
		       expect(fake.log_messages[1].contains(std::strerror(EIO)),
		              std::string(name) + " second log includes destroy strerror") &&
		       expect(fake.log_messages[1].contains(std::to_string(EIO)),
		              std::string(name) + " second log includes destroy numeric error");
	}

	auto expect_child_reaped(AuthHelperSpawnFake *fake, std::string_view name) -> bool {
		if (fake->spawned_pid < 0) {
			return true;
		}

		int status             = 0;
		errno                  = 0;
		const auto wait_result = waitpid(fake->spawned_pid, &status, WNOHANG);
		if (wait_result == fake->spawned_pid) {
			return expect(false, std::string(name) + " leaves no unreaped helper child");
		}
		if (wait_result == 0) {
			(void)waitpid(fake->spawned_pid, &status, 0);
			return expect(false, std::string(name) + " leaves no active helper child");
		}
		return expect(wait_result == -1 && errno == ECHILD,
		              std::string(name) + " reaps helper child");
	}

	auto test_auth_helper_spawn_setup_failures() -> bool {
		enum class FailurePoint {
			kInit,
			kFirstClose,
			kStdoutDup,
			kStderrDup,
			kFinalWriteClose,
		};

		struct TestCase {
			std::string_view         name;
			FailurePoint             point;
			std::string_view         operation;
			std::size_t              expected_dup_calls;
			std::size_t              expected_close_actions;
			int                      expected_destroy_calls;
			std::vector<std::string> expected_actions;
		};

		const std::vector<TestCase> test_cases = {
		    {
		        .name                   = "actions init",
		        .point                  = FailurePoint::kInit,
		        .operation              = "posix_spawn_file_actions_init",
		        .expected_dup_calls     = 0,
		        .expected_close_actions = 0,
		        .expected_destroy_calls = 0,
		        .expected_actions       = {"actions_init"},
		    },
		    {
		        .name                   = "first child close",
		        .point                  = FailurePoint::kFirstClose,
		        .operation              = "posix_spawn_file_actions_addclose",
		        .expected_dup_calls     = 0,
		        .expected_close_actions = 1,
		        .expected_destroy_calls = 1,
		        .expected_actions = {"actions_init", "actions_addclose_read", "actions_destroy"},
		    },
		    {
		        .name                   = "stdout duplication",
		        .point                  = FailurePoint::kStdoutDup,
		        .operation              = "posix_spawn_file_actions_adddup2",
		        .expected_dup_calls     = 1,
		        .expected_close_actions = 1,
		        .expected_destroy_calls = 1,
		        .expected_actions       = {"actions_init", "actions_addclose_read",
		                                   "actions_adddup2_stdout", "actions_destroy"},
		    },
		    {
		        .name                   = "stderr duplication",
		        .point                  = FailurePoint::kStderrDup,
		        .operation              = "posix_spawn_file_actions_adddup2",
		        .expected_dup_calls     = 2,
		        .expected_close_actions = 1,
		        .expected_destroy_calls = 1,
		        .expected_actions       = {"actions_init", "actions_addclose_read",
		                                   "actions_adddup2_stdout", "actions_adddup2_stderr",
		                                   "actions_destroy"},
		    },
		    {
		        .name                   = "final child write close",
		        .point                  = FailurePoint::kFinalWriteClose,
		        .operation              = "posix_spawn_file_actions_addclose",
		        .expected_dup_calls     = 2,
		        .expected_close_actions = 2,
		        .expected_destroy_calls = 1,
		        .expected_actions       = {"actions_init", "actions_addclose_read",
		                                   "actions_adddup2_stdout", "actions_adddup2_stderr",
		                                   "actions_addclose_write", "actions_destroy"},
		    },
		};

		bool ok = true;
		for (const auto &test_case : test_cases) {
			AuthHelperSpawnFake fake;
			switch (test_case.point) {
				case FailurePoint::kInit:
					fake.actions_init_result = EIO;
					break;
				case FailurePoint::kFirstClose:
					fake.first_close_result = EIO;
					break;
				case FailurePoint::kStdoutDup:
					fake.stdout_dup_result = EIO;
					break;
				case FailurePoint::kStderrDup:
					fake.stderr_dup_result = EIO;
					break;
				case FailurePoint::kFinalWriteClose:
					fake.final_close_result = EIO;
					break;
			}

			ScopedAuthHelperSpawnHooks       hooks(&fake);
			howdy::pam::PreparedRuntimeFiles prepared;
			const bool prepared_ok = howdy::pam::testing::prepare_runtime_auth_files(
			    "alice", &prepared, spawn_operations(&fake));
			ok &= expect(!prepared_ok, std::string(test_case.name) + " returns false");
			ok &= expect(fake.spawn_calls == 0,
			             std::string(test_case.name) + " does not spawn helper");
			ok &= expect(fake.dup2_fds.size() == test_case.expected_dup_calls,
			             std::string(test_case.name) + " runs no later duplicate actions");
			ok &= expect(fake.action_close_fds.size() == test_case.expected_close_actions,
			             std::string(test_case.name) + " runs no later close actions");
			ok &= expect(fake.actions_destroy_calls == test_case.expected_destroy_calls,
			             std::string(test_case.name) + " follows action destroy rule");
			ok &= expect(action_operations(fake) == test_case.expected_actions,
			             std::string(test_case.name) + " runs no later spawn actions");
			ok &= expect_parent_pipe_closed_once(fake, test_case.name);
			ok &= expect_primary_spawn_error(fake, test_case.operation, EIO, test_case.name);
		}
		return ok;
	}

	auto test_auth_helper_spawn_setup_failure_destroy_failure() -> bool {
		AuthHelperSpawnFake fake{
		    .stdout_dup_result      = EIO,
		    .actions_destroy_result = EIO,
		};
		ScopedAuthHelperSpawnHooks       hooks(&fake);
		howdy::pam::PreparedRuntimeFiles prepared;
		const bool prepared_ok = howdy::pam::testing::prepare_runtime_auth_files(
		    "alice", &prepared, spawn_operations(&fake));
		return expect(!prepared_ok, "setup and destroy failure returns false") &&
		       expect(fake.spawn_calls == 0, "setup and destroy failure does not spawn helper") &&
		       expect(fake.spawned_pid == -1, "setup and destroy failure creates no child") &&
		       expect(fake.actions_destroy_calls == 1,
		              "setup and destroy failure destroys actions once") &&
		       expect_parent_pipe_closed_once(fake, "setup and destroy failure") &&
		       expect_primary_spawn_error(fake, "posix_spawn_file_actions_adddup2", EIO,
		                                  "setup and destroy failure") &&
		       expect_destroy_error_after_primary(fake, "setup and destroy failure");
	}

	auto test_auth_helper_spawn_failure_destroy_failure() -> bool {
		AuthHelperSpawnFake fake{
		    .actions_destroy_result = EIO,
		    .spawn_result           = EAGAIN,
		};
		ScopedAuthHelperSpawnHooks       hooks(&fake);
		howdy::pam::PreparedRuntimeFiles prepared;
		const bool prepared_ok = howdy::pam::testing::prepare_runtime_auth_files(
		    "alice", &prepared, spawn_operations(&fake));
		return expect(!prepared_ok, "spawn and destroy failure returns false") &&
		       expect(fake.spawn_calls == 1, "spawn and destroy failure calls spawn once") &&
		       expect(fake.output_reader_calls == 0,
		              "spawn and destroy failure does not read helper output") &&
		       expect(fake.spawned_pid == -1, "spawn and destroy failure creates no child") &&
		       expect(fake.actions_destroy_calls == 1,
		              "spawn and destroy failure destroys actions once") &&
		       expect_parent_pipe_closed_once(fake, "spawn and destroy failure") &&
		       expect_primary_spawn_error(fake, "posix_spawn", EAGAIN,
		                                  "spawn and destroy failure") &&
		       expect_destroy_error_after_primary(fake, "spawn and destroy failure");
	}

	auto test_auth_helper_spawn_destroy_failure() -> bool {
		AuthHelperSpawnFake              fake{.actions_destroy_result = EIO};
		ScopedAuthHelperSpawnHooks       hooks(&fake);
		howdy::pam::PreparedRuntimeFiles prepared;
		const bool prepared_ok = howdy::pam::testing::prepare_runtime_auth_files(
		    "alice", &prepared, spawn_operations(&fake));
		return expect(prepared_ok, "post-spawn destroy failure preserves helper success") &&
		       expect(fake.spawn_calls == 1, "destroy failure occurs after helper spawn") &&
		       expect(fake.output_reader_calls == 1, "destroy failure still reads helper output") &&
		       expect(fake.actions_destroy_calls == 1, "destroy failure destroys actions once") &&
		       expect_parent_pipe_closed_once(fake, "destroy failure") &&
		       expect(prepared.config_path == "/run/howdy/auth-helper/config.ini",
		              "destroy failure preserves prepared config path") &&
		       expect(prepared.user_models_dir == "/run/howdy/auth-helper/models",
		              "destroy failure preserves prepared models path") &&
		       expect(prepared.root_dir == "/run/howdy/auth-helper",
		              "destroy failure preserves prepared root path") &&
		       expect_primary_spawn_error(fake, "posix_spawn_file_actions_destroy", EIO,
		                                  "destroy failure") &&
		       expect_child_reaped(&fake, "destroy failure");
	}

	auto test_auth_helper_spawn_failure() -> bool {
		AuthHelperSpawnFake              fake{.spawn_result = EAGAIN};
		ScopedAuthHelperSpawnHooks       hooks(&fake);
		howdy::pam::PreparedRuntimeFiles prepared;
		const bool prepared_ok = howdy::pam::testing::prepare_runtime_auth_files(
		    "alice", &prepared, spawn_operations(&fake));
		return expect(!prepared_ok, "spawn failure returns false") &&
		       expect(fake.spawn_calls == 1, "spawn failure calls spawn once") &&
		       expect(fake.output_reader_calls == 0, "spawn failure does not read helper output") &&
		       expect(fake.actions_destroy_calls == 1, "spawn failure destroys actions once") &&
		       expect_parent_pipe_closed_once(fake, "spawn failure") &&
		       expect_primary_spawn_error(fake, "posix_spawn", EAGAIN, "spawn failure");
	}

	auto test_auth_helper_spawn_success() -> bool {
		AuthHelperSpawnFake              fake;
		ScopedAuthHelperSpawnHooks       hooks(&fake);
		howdy::pam::PreparedRuntimeFiles prepared;
		const bool prepared_ok = howdy::pam::testing::prepare_runtime_auth_files(
		    "alice", &prepared, spawn_operations(&fake));
		return expect(prepared_ok, "spawn success returns true") &&
		       expect(fake.pipe_fds == std::vector<std::array<int, 2>>{{10, 11}},
		              "spawn success uses expected pipe fds") &&
		       expect(fake.pipe_flags == std::vector<int>{O_CLOEXEC},
		              "spawn success creates close-on-exec pipe") &&
		       expect(fake.spawn_path == kAuthHelperPath, "spawn success uses auth helper path") &&
		       expect(fake.spawn_argv ==
		                  std::vector<std::string>{kAuthHelperPath, "prepare", "alice"},
		              "spawn success uses expected argv") &&
		       expect(fake.spawn_env.empty(), "spawn success uses empty environment") &&
		       expect(fake.dup2_fds == std::vector<std::pair<int, int>>{{11, STDOUT_FILENO},
		                                                                {11, STDERR_FILENO}},
		              "spawn success redirects stdout and stderr") &&
		       expect(fake.action_close_fds == std::vector<int>{10, 11},
		              "spawn success closes both pipe fds in child") &&
		       expect(action_operations(fake) ==
		                  std::vector<std::string>{"actions_init", "actions_addclose_read",
		                                           "actions_adddup2_stdout",
		                                           "actions_adddup2_stderr",
		                                           "actions_addclose_write", "actions_destroy"},
		              "spawn success performs child actions in order") &&
		       expect(fake.actions_destroy_calls == 1, "spawn success destroys actions once") &&
		       expect(fake.output_reader_calls == 1, "spawn success reads helper output once") &&
		       expect(fake.parent_close_fds == std::vector<int>{11, 10},
		              "spawn success closes parent write then read fd") &&
		       expect(
		           fake.operations ==
		               std::vector<std::string>{"pipe2", "actions_init", "actions_addclose_read",
		                                        "actions_adddup2_stdout", "actions_adddup2_stderr",
		                                        "actions_addclose_write", "spawn",
		                                        "actions_destroy", "close", "read_output", "close"},
		           "spawn success performs helper operations in order") &&
		       expect_parent_pipe_closed_once(fake, "spawn success") &&
		       expect(prepared.config_path == "/run/howdy/auth-helper/config.ini",
		              "spawn success reads config path") &&
		       expect(prepared.user_models_dir == "/run/howdy/auth-helper/models",
		              "spawn success reads models path") &&
		       expect(prepared.root_dir == "/run/howdy/auth-helper",
		              "spawn success derives runtime root") &&
		       expect_child_reaped(&fake, "spawn success");
	}

	auto integration_pipe2(void *context, int *pipe_fds, int flags) -> int {
		(void)context;
		const int result = pipe2(pipe_fds, flags);
		if (result != 0 || (pipe_fds[0] == STDOUT_FILENO && pipe_fds[1] == STDERR_FILENO)) {
			return result;
		}
		(void)close(pipe_fds[0]);
		(void)close(pipe_fds[1]);
		errno = EBUSY;
		return -1;
	}

	auto integration_duplicate_fd(void *context, int fd, int minimum_fd) -> int {
		(void)context;
		return fcntl(fd, F_DUPFD_CLOEXEC, minimum_fd);
	}

	auto integration_actions_init(void *context, posix_spawn_file_actions_t *actions) -> int {
		(void)context;
		return posix_spawn_file_actions_init(actions);
	}

	auto integration_actions_adddup2(void *context, posix_spawn_file_actions_t *actions,
	                                 int source_fd, int target_fd) -> int {
		(void)context;
		return posix_spawn_file_actions_adddup2(actions, source_fd, target_fd);
	}

	auto integration_actions_addclose(void *context, posix_spawn_file_actions_t *actions, int fd)
	    -> int {
		(void)context;
		return posix_spawn_file_actions_addclose(actions, fd);
	}

	auto integration_actions_destroy(void *context, posix_spawn_file_actions_t *actions) -> int {
		(void)context;
		return posix_spawn_file_actions_destroy(actions);
	}

	auto integration_spawn(void *context, pid_t *child_pid, const char *path,
	                       const posix_spawn_file_actions_t *actions, char *const *argv,
	                       char *const *envp) -> int {
		(void)context;
		(void)path;
		(void)argv;
		(void)envp;
		std::array<char *, 4> shell_args = {
		    const_cast<char *>("/bin/sh"),
		    const_cast<char *>("-c"),
		    const_cast<char *>("printf 'CONFIG_PATH=/run/howdy/collision/config.ini\\n'; "
		                       "printf 'USER_MODELS_DIR=/run/howdy/collision/models\\n' >&2"),
		    nullptr,
		};
		std::array<char *, 1> empty_env = {nullptr};
		return posix_spawn(child_pid, "/bin/sh", actions, nullptr, shell_args.data(),
		                   empty_env.data());
	}

	auto integration_close(void *context, int fd) -> int {
		(void)context;
		return close(fd);
	}

	auto test_auth_helper_spawn_real_descriptor_collisions() -> bool {
		const pid_t test_pid = fork();
		if (test_pid == 0) {
			(void)close(STDOUT_FILENO);
			(void)close(STDERR_FILENO);
			howdy::pam::PreparedRuntimeFiles                     prepared;
			const howdy::pam::testing::AuthHelperSpawnOperations operations{
			    .pipe2_fn            = integration_pipe2,
			    .duplicate_fd_fn     = integration_duplicate_fd,
			    .actions_init_fn     = integration_actions_init,
			    .actions_adddup2_fn  = integration_actions_adddup2,
			    .actions_addclose_fn = integration_actions_addclose,
			    .actions_destroy_fn  = integration_actions_destroy,
			    .spawn_fn            = integration_spawn,
			    .close_fn            = integration_close,
			};
			const bool prepared_ok =
			    howdy::pam::testing::prepare_runtime_auth_files("alice", &prepared, operations);
			const bool paths_ok = prepared.config_path == "/run/howdy/collision/config.ini" &&
			                      prepared.user_models_dir == "/run/howdy/collision/models" &&
			                      prepared.root_dir == "/run/howdy/collision";
			_exit(prepared_ok && paths_ok ? EXIT_SUCCESS : EXIT_FAILURE);
		}
		if (test_pid < 0) {
			return expect(false, "real descriptor collision test forks subprocess");
		}

		int status = 0;
		while (waitpid(test_pid, &status, 0) < 0) {
			if (errno != EINTR) {
				return expect(false, "real descriptor collision test waits for subprocess");
			}
		}
		return expect(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS,
		              "real file actions preserve stdout and stderr across exec");
	}

	auto test_auth_helper_spawn_descriptor_collisions() -> bool {
		const std::vector<std::array<int, 2>> pipe_fd_pairs = {
		    {STDOUT_FILENO, STDERR_FILENO},
		    {STDERR_FILENO, STDOUT_FILENO},
		};

		bool ok = true;
		for (const auto &pipe_fds : pipe_fd_pairs) {
			AuthHelperSpawnFake              fake{.next_pipe_fds = pipe_fds};
			ScopedAuthHelperSpawnHooks       hooks(&fake);
			howdy::pam::PreparedRuntimeFiles prepared;
			const bool prepared_ok = howdy::pam::testing::prepare_runtime_auth_files(
			    "alice", &prepared, spawn_operations(&fake));
			const char *const name =
			    pipe_fds[0] == STDOUT_FILENO ? "stdout-read collision" : "stderr-read collision";
			ok &= expect(prepared_ok, std::string(name) + " succeeds");
			ok &= expect(fake.duplicate_fd_requests ==
			                 std::vector<std::pair<int, int>>{{pipe_fds[0], STDERR_FILENO + 1},
			                                                  {pipe_fds[1], STDERR_FILENO + 1}},
			             std::string(name) + " normalizes both pipe descriptors");
			ok &= expect(fake.action_close_fds == std::vector<int>{20, 21},
			             std::string(name) + " closes normalized pipe fds in child");
			ok &= expect(action_operations(fake) ==
			                 std::vector<std::string>{"actions_init", "actions_addclose_read",
			                                          "actions_adddup2_stdout",
			                                          "actions_adddup2_stderr",
			                                          "actions_addclose_write", "actions_destroy"},
			             std::string(name) + " uses collision-free child actions");
			ok &= expect(fake.dup2_fds == std::vector<std::pair<int, int>>{{21, STDOUT_FILENO},
			                                                               {21, STDERR_FILENO}},
			             std::string(name) + " redirects normalized write fd");
			ok &=
			    expect(fake.parent_close_fds == std::vector<int>{pipe_fds[0], pipe_fds[1], 21, 20},
			           std::string(name) + " closes original and normalized fds once");
			ok &= expect_parent_pipe_closed_once(fake, name);
			ok &= expect_child_reaped(&fake, name);
		}
		return ok;
	}

	auto test_invalid_prepared_runtime_files() -> bool {
		struct TestCase {
			std::string_view                 name;
			howdy::pam::PreparedRuntimeFiles prepared;
			bool                             cleanup_expected = false;
		};

		const std::vector<TestCase> test_cases = {
		    {
		        .name = "empty config path",
		        .prepared{
		            .root_dir        = "/run/howdy/invalid-config",
		            .config_path     = "",
		            .user_models_dir = "/run/howdy/invalid-config/models",
		        },
		        .cleanup_expected = true,
		    },
		    {
		        .name = "empty models path",
		        .prepared{
		            .root_dir        = "/run/howdy/invalid-models",
		            .config_path     = "/run/howdy/invalid-models/config.ini",
		            .user_models_dir = "",
		        },
		        .cleanup_expected = true,
		    },
		    {
		        .name = "empty root path",
		        .prepared{
		            .root_dir        = "",
		            .config_path     = "/run/howdy/invalid-root/config.ini",
		            .user_models_dir = "/run/howdy/invalid-root/models",
		        },
		    },
		};

		bool ok = true;
		for (const auto &test_case : test_cases) {
			FakeContext context{
			    .load_results  = {path_error_result(EACCES)},
			    .effective_uid = 1000,
			    .prepared      = test_case.prepared,
			};
			{
				howdy::pam::RuntimeSession session(kConfiguredConfig, kConfiguredModels,
				                                   dependencies(&context));
				const auto                 result = session.load_for_user("alice");
				ok &= expect(result.status == howdy::pam::RuntimeSessionLoadStatus::kPrepareFailed,
				             std::string(test_case.name) + " is rejected");
				ok &= expect(context.load_paths.size() == 1,
				             std::string(test_case.name) + " does not load staged config");
				ok &= expect(!session.staged(), std::string(test_case.name) + " is not staged");
				ok &= expect(session.config_path() == kConfiguredConfig,
				             std::string(test_case.name) + " preserves configured config path");
				ok &= expect(session.user_models_dir() == kConfiguredModels,
				             std::string(test_case.name) + " preserves configured models path");
				const auto expected_cleanup_count = test_case.cleanup_expected ? 1U : 0U;
				ok &= expect(context.cleanup_roots.size() == expected_cleanup_count,
				             std::string(test_case.name) + " has expected immediate cleanup");
				if (test_case.cleanup_expected && !context.cleanup_roots.empty()) {
					ok &= expect(context.cleanup_roots.front() == test_case.prepared.root_dir,
					             std::string(test_case.name) + " cleans prepared root");
				}
			}
			const auto expected_cleanup_count = test_case.cleanup_expected ? 1U : 0U;
			ok &= expect(context.cleanup_roots.size() == expected_cleanup_count,
			             std::string(test_case.name) + " does not clean twice at destruction");
		}
		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= test_direct_success();
	ok &= test_direct_parse_failure();
	ok &= test_root_eacces_failure();
	ok &= test_non_root_non_eacces_failure();
	ok &= test_staged_success();
	ok &= test_prepare_failure();
	ok &= test_staged_config_failure();
	ok &= test_missing_dependencies();
	ok &= test_no_duplicate_cleanup();
	ok &= test_direct_success_is_one_shot();
	ok &= test_staged_success_is_one_shot();
	ok &= test_failed_staged_load_is_one_shot();
	ok &= test_invalid_dependencies_are_one_shot();
	ok &= test_invalid_prepared_runtime_files();
	ok &= test_auth_helper_spawn_setup_failures();
	ok &= test_auth_helper_spawn_setup_failure_destroy_failure();
	ok &= test_auth_helper_spawn_failure_destroy_failure();
	ok &= test_auth_helper_spawn_destroy_failure();
	ok &= test_auth_helper_spawn_failure();
	ok &= test_auth_helper_spawn_success();
	ok &= test_auth_helper_spawn_descriptor_collisions();
	ok &= test_auth_helper_spawn_real_descriptor_collisions();
	return ok ? 0 : 1;
}
