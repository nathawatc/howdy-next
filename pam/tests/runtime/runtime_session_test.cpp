#include "protocol/auth_helper_protocol.hpp"
#include "runtime/runtime_session.hpp"
#include "runtime/runtime_session_test_groups.hpp"
#include "test_support.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <paths.hpp>
#include <spawn.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include <sys/wait.h>

namespace {

	using howdy::test::expect;

	constexpr auto kConfiguredConfig = "/etc/howdy/config.ini";
	constexpr auto kConfiguredModels = "/var/lib/howdy/models";

	auto make_prepared_runtime(std::string_view suffix) -> howdy::pam::PreparedRuntimeFiles {
		const auto root =
		    howdy::native::auth_helper_protocol::prepared_runtime_root() /
		    (howdy::native::auth_helper_protocol::prepared_runtime_directory_prefix(getuid()) +
		     std::string(suffix));
		return {
		    .root_dir    = root,
		    .config_path = howdy::native::auth_helper_protocol::prepared_config_path(root).string(),
		    .user_models_dir =
		        howdy::native::auth_helper_protocol::prepared_user_models_dir(root).string(),
		};
	}

	struct FakeContext {
		std::vector<howdy::native::RuntimeConfigLoadResult> load_results;
		std::vector<std::filesystem::path>                  load_paths;
		std::vector<std::string>                            prepare_usernames;
		std::vector<std::filesystem::path>                  cleanup_roots;
		int                                                 effective_uid_calls = 0;
		bool                                                prepare_succeeds    = true;
		uid_t                                               effective_uid       = 0;
		howdy::pam::PreparedRuntimeFiles prepared = make_prepared_runtime("stage1");
	};

	struct CallbackCounts {
		std::size_t load          = 0;
		std::size_t prepare       = 0;
		std::size_t cleanup       = 0;
		int         effective_uid = 0;

		auto operator==(const CallbackCounts &) const -> bool = default;
	};

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
		const auto  staged_runtime = make_prepared_runtime("stage1");
		FakeContext context{
		    .load_results = {path_error_result(EACCES), success_result(staged_runtime.config_path)},
		    .effective_uid = 1000,
		};
		bool ok = true;
		{
			howdy::pam::RuntimeSession session(kConfiguredConfig, kConfiguredModels,
			                                   dependencies(&context));
			const auto                 result = session.load_for_user("alice");
			ok &= expect(result.ok(), "staged config success returns ok");
			ok &= expect(context.load_paths ==
			                 std::vector<std::filesystem::path>{kConfiguredConfig,
			                                                    staged_runtime.config_path},
			             "staged success loads configured then staged path");
			ok &= expect(context.prepare_usernames == std::vector<std::string>{"alice"},
			             "staged success prepares once for alice");
			ok &= expect(session.staged(), "staged success reports staged");
			ok &= expect(session.config_path() == staged_runtime.config_path,
			             "staged success uses staged config");
			ok &= expect(session.user_models_dir() == staged_runtime.user_models_dir,
			             "staged success uses staged models");
			ok &= expect(context.cleanup_roots.empty(), "staged success defers cleanup");
		}
		ok &= expect(context.cleanup_roots ==
		                 std::vector<std::filesystem::path>{staged_runtime.root_dir},
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
		const auto  staged_runtime = make_prepared_runtime("stage1");
		FakeContext context{
		    .load_results  = {path_error_result(EACCES),
		                      parse_error_result(staged_runtime.config_path)},
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
		ok &= expect(context.cleanup_roots ==
		                 std::vector<std::filesystem::path>{staged_runtime.root_dir},
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
		const auto  staged_runtime = make_prepared_runtime("stage1");
		FakeContext context{
		    .load_results = {path_error_result(EACCES), success_result(staged_runtime.config_path)},
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
		const auto  runtime = make_prepared_runtime("run001");
		FakeContext context{
		    .load_results  = {path_error_result(EACCES), success_result(runtime.config_path)},
		    .effective_uid = 1000,
		    .prepared      = runtime,
		};
		bool ok = true;
		{
			howdy::pam::RuntimeSession session(kConfiguredConfig, kConfiguredModels,
			                                   dependencies(&context));
			const auto                 first = session.load_for_user("alice");
			ok &= expect(first.status == howdy::pam::RuntimeSessionLoadStatus::kOk,
			             "staged one-shot first load succeeds");
			ok &= expect(session.config_path() == runtime.config_path,
			             "staged one-shot activates runtime-a config");
			ok &= expect(session.user_models_dir() == runtime.user_models_dir,
			             "staged one-shot activates runtime-a models");
			const auto counts_after_first = callback_counts(context);

			const auto second = session.load_for_user("bob");
			ok &= expect(second.status == howdy::pam::RuntimeSessionLoadStatus::kAlreadyLoaded,
			             "staged one-shot rejects second load");
			ok &= expect(callback_counts(context) == counts_after_first,
			             "staged re-entry invokes no callbacks");
			ok &= expect(session.config_path() == runtime.config_path,
			             "staged re-entry preserves runtime-a config");
			ok &= expect(session.user_models_dir() == runtime.user_models_dir,
			             "staged re-entry preserves runtime-a models");
		}
		ok &= expect(context.cleanup_roots == std::vector<std::filesystem::path>{runtime.root_dir},
		             "staged re-entry cleans runtime-a exactly once");
		return ok;
	}

	auto test_failed_staged_load_is_one_shot() -> bool {
		const auto  runtime = make_prepared_runtime("fail01");
		FakeContext context{
		    .load_results  = {path_error_result(EACCES), parse_error_result(runtime.config_path)},
		    .effective_uid = 1000,
		    .prepared      = runtime,
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
		ok &= expect(context.cleanup_roots == std::vector<std::filesystem::path>{runtime.root_dir},
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

	auto test_invalid_prepared_runtime_files() -> bool {
		namespace fs = std::filesystem;
		using howdy::native::auth_helper_protocol::prepared_user_models_dir;

		struct TestCase {
			std::string_view                 name;
			howdy::pam::PreparedRuntimeFiles prepared;
			bool                             cleanup_expected = false;
		};

		const auto valid   = make_prepared_runtime("valid1");
		const auto sibling = make_prepared_runtime("sibling");

		auto empty_config = valid;
		empty_config.config_path.clear();
		auto empty_models = valid;
		empty_models.user_models_dir.clear();
		auto empty_root = valid;
		empty_root.root_dir.clear();

		auto config_outside            = valid;
		config_outside.config_path     = "/etc/howdy/config.ini";
		auto models_outside            = valid;
		models_outside.user_models_dir = "/var/lib/howdy/models";
		auto sibling_models            = valid;
		sibling_models.user_models_dir = prepared_user_models_dir(sibling.root_dir).string();
		auto swapped_paths             = valid;
		swapped_paths.config_path      = valid.user_models_dir;
		swapped_paths.user_models_dir  = valid.config_path;
		auto nested_config             = valid;
		nested_config.config_path = (fs::path(valid.root_dir) / "nested" / "config.ini").string();
		auto escaped_models       = valid;
		escaped_models.user_models_dir =
		    (fs::path(valid.root_dir) / "models" / ".." / "models").string();
		auto noncanonical_root = valid;
		noncanonical_root.root_dir =
		    fs::path(valid.root_dir).parent_path() / ".." / fs::path(valid.root_dir).filename();
		auto wrong_config_name            = valid;
		wrong_config_name.config_path     = (fs::path(valid.root_dir) / "settings.ini").string();
		auto wrong_models_name            = valid;
		wrong_models_name.user_models_dir = (fs::path(valid.root_dir) / "user-models").string();
		auto relative_paths               = valid;
		relative_paths.config_path        = "relative/config.ini";
		relative_paths.user_models_dir    = "relative/models";
		auto outside_root                 = valid;
		outside_root.root_dir             = "/tmp/howdy-auth-helper-runtime";
		outside_root.config_path          = "/tmp/howdy-auth-helper-runtime/config.ini";
		outside_root.user_models_dir      = "/tmp/howdy-auth-helper-runtime/models";

		const std::vector<TestCase> test_cases = {
		    {.name = "empty config path", .prepared = empty_config, .cleanup_expected = true},
		    {.name = "empty models path", .prepared = empty_models, .cleanup_expected = true},
		    {.name = "empty root path", .prepared = empty_root},
		    {.name             = "config path outside prepared runtime root",
		     .prepared         = config_outside,
		     .cleanup_expected = true},
		    {.name             = "user-model directory outside prepared runtime root",
		     .prepared         = models_outside,
		     .cleanup_expected = true},
		    {.name             = "sibling path with valid-looking basename",
		     .prepared         = sibling_models,
		     .cleanup_expected = true},
		    {.name             = "invalid prepared runtime suffix",
		     .prepared         = sibling,
		     .cleanup_expected = true},
		    {.name             = "config and models paths swapped",
		     .prepared         = swapped_paths,
		     .cleanup_expected = true},
		    {.name             = "unexpected nested config path",
		     .prepared         = nested_config,
		     .cleanup_expected = true},
		    {.name             = "models path with parent traversal",
		     .prepared         = escaped_models,
		     .cleanup_expected = true},
		    {.name             = "equivalent noncanonical runtime root",
		     .prepared         = noncanonical_root,
		     .cleanup_expected = true},
		    {.name             = "unexpected config basename",
		     .prepared         = wrong_config_name,
		     .cleanup_expected = true},
		    {.name             = "unexpected models basename",
		     .prepared         = wrong_models_name,
		     .cleanup_expected = true},
		    {.name             = "relative prepared paths",
		     .prepared         = relative_paths,
		     .cleanup_expected = true},
		    {.name             = "runtime root outside /run/howdy",
		     .prepared         = outside_root,
		     .cleanup_expected = true},
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
				const std::string          name(test_case.name);
				ok &= expect(result.status == howdy::pam::RuntimeSessionLoadStatus::kPrepareFailed,
				             name + " is rejected");
				ok &= expect(context.load_paths.size() == 1, name + " does not load staged config");
				ok &= expect(!session.staged(), name + " is not staged");
				ok &= expect(session.config_path() == kConfiguredConfig,
				             name + " preserves configured config path");
				ok &= expect(session.user_models_dir() == kConfiguredModels,
				             name + " preserves configured models path");
				const auto expected_cleanup_count = test_case.cleanup_expected ? 1U : 0U;
				ok &= expect(context.cleanup_roots.size() == expected_cleanup_count,
				             name + " has expected immediate cleanup");
				if (test_case.cleanup_expected && !context.cleanup_roots.empty()) {
					ok &= expect(context.cleanup_roots.front() == test_case.prepared.root_dir,
					             name + " cleans prepared root");
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
	ok &= run_runtime_session_spawn_tests();
	ok &= run_runtime_session_deadline_tests();
	ok &= test_invalid_prepared_runtime_files();
	return ok ? 0 : 1;
}
