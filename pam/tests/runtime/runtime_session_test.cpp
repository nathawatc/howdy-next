#include "protocol/auth_helper_protocol.hpp"
#include "runtime/runtime_session.hpp"
#include "runtime/runtime_session_test_groups.hpp"
#include "test_support.hpp"

#include <array>
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

	auto MakePreparedRuntime(std::string_view suffix) -> howdy::pam::PreparedRuntimeFiles {
		const auto root =
		    suffix == "sibling"
		        ? howdy::native::auth_helper_protocol::PreparedRuntimeRoot() /
		              (howdy::native::auth_helper_protocol::PreparedRuntimeDirectoryPrefix(
		                   getuid()) +
		               std::string(suffix))
		        : howdy::native::auth_helper_protocol::PreparedRuntimeGenerationDir(
		              howdy::native::auth_helper_protocol::PreparedRuntimeRoot(), getuid(),
		              howdy::native::auth_helper_protocol::RuntimeGenerationSlot::kSlot0);
		return {
		    .root_dir    = root,
		    .config_path = howdy::native::auth_helper_protocol::PreparedConfigPath(root).string(),
		    .user_models_dir =
		        howdy::native::auth_helper_protocol::PreparedUserModelsDir(root).string(),
		};
	}

	struct FakeContext {
		std::vector<howdy::native::RuntimeConfigLoadResult> load_results;
		std::vector<std::filesystem::path>                  load_paths;
		std::vector<std::string>                            prepare_usernames;
		std::vector<int>                                    issued_lease_fds;
		int                                                 effective_uid_calls = 0;
		bool                                                prepare_succeeds    = true;
		uid_t                                               effective_uid       = 0;
		howdy::pam::PreparedRuntimeFiles prepared = MakePreparedRuntime("stage1");
	};

	struct CallbackCounts {
		std::size_t load          = 0;
		std::size_t prepare       = 0;
		int         effective_uid = 0;

		auto operator==(const CallbackCounts &) const -> bool = default;
	};

	auto SuccessResult(const std::filesystem::path &path)
	    -> howdy::native::RuntimeConfigLoadResult {
		return howdy::native::RuntimeConfigLoadResult{
		    .ok     = true,
		    .status = howdy::native::RuntimeConfigLoadStatus::kOk,
		    .path   = path,
		    .config = howdy::native::RuntimeConfig{},
		};
	}

	auto PathErrorResult(int error_code) -> howdy::native::RuntimeConfigLoadResult {
		return howdy::native::RuntimeConfigLoadResult{
		    .status        = howdy::native::RuntimeConfigLoadStatus::kPathError,
		    .path          = kConfiguredConfig,
		    .error_message = "path error",
		    .error_code    = error_code,
		};
	}

	auto ParseErrorResult(const std::filesystem::path &path)
	    -> howdy::native::RuntimeConfigLoadResult {
		return howdy::native::RuntimeConfigLoadResult{
		    .status        = howdy::native::RuntimeConfigLoadStatus::kParseError,
		    .path          = path,
		    .error_message = "parse error",
		};
	}

	auto PrepareRuntime(void *context, std::string_view username,
	                    howdy::pam::PreparedRuntimeFiles *prepared) -> bool {
		auto &fake = *static_cast<FakeContext *>(context);
		fake.prepare_usernames.emplace_back(username);
		if (!fake.prepare_succeeds) {
			return false;
		}
		std::array<int, 2> lease_pipe{};
		if (pipe2(lease_pipe.data(), O_CLOEXEC) != 0) {
			return false;
		}
		(void)close(lease_pipe[1]);
		*prepared          = fake.prepared;
		prepared->lease_fd = lease_pipe[0];
		fake.issued_lease_fds.push_back(lease_pipe[0]);
		return true;
	}

	auto LoadRuntimeConfig(void *context, const std::filesystem::path &config_path)
	    -> howdy::native::RuntimeConfigLoadResult {
		auto &fake = *static_cast<FakeContext *>(context);
		fake.load_paths.push_back(config_path);
		const auto result_index = fake.load_paths.size() - 1;
		if (result_index >= fake.load_results.size()) {
			return ParseErrorResult(config_path);
		}
		return fake.load_results[result_index];
	}

	auto EffectiveUid(void *context) -> uid_t {
		auto &fake = *static_cast<FakeContext *>(context);
		++fake.effective_uid_calls;
		return fake.effective_uid;
	}

	auto Dependencies(FakeContext *context) -> howdy::pam::RuntimeSessionDependencies {
		return howdy::pam::RuntimeSessionDependencies{
		    .context             = context,
		    .prepare_runtime     = PrepareRuntime,
		    .load_runtime_config = LoadRuntimeConfig,
		    .effective_uid       = EffectiveUid,
		};
	}

	auto IssuedLeaseHasState(const FakeContext &context, bool open) -> bool {
		if (context.issued_lease_fds.empty()) {
			return false;
		}
		errno              = 0;
		const bool is_open = fcntl(context.issued_lease_fds.back(), F_GETFD) >= 0;
		return is_open == open && (is_open || errno == EBADF);
	}

	auto GetCallbackCounts(const FakeContext &context) -> CallbackCounts {
		return CallbackCounts{
		    .load          = context.load_paths.size(),
		    .prepare       = context.prepare_usernames.size(),
		    .effective_uid = context.effective_uid_calls,
		};
	}

	auto NoCallbacksRan(const FakeContext &context) -> bool {
		return context.load_paths.empty() && context.prepare_usernames.empty() &&
		       context.effective_uid_calls == 0;
	}

	auto TestDirectSuccess() -> bool {
		FakeContext context{.load_results = {SuccessResult(kConfiguredConfig)}};
		bool        ok = true;
		{
			howdy::pam::RuntimeSession session(kConfiguredConfig, kConfiguredModels,
			                                   Dependencies(&context));
			const auto                 result = session.LoadForUser("alice");
			ok &= expect(result.Ok(), "direct success returns ok");
			ok &=
			    expect(context.load_paths == std::vector<std::filesystem::path>{kConfiguredConfig},
			           "direct success loads configured path once");
			ok &= expect(context.prepare_usernames.empty(), "direct success does not prepare");
			ok &= expect(!session.Staged(), "direct success is not staged");
			ok &= expect(session.ConfigPath() == kConfiguredConfig,
			             "direct success retains configured config path");
			ok &= expect(session.UserModelsDir() == kConfiguredModels,
			             "direct success retains configured models path");
		}
		return ok;
	}

	auto TestDirectParseFailure() -> bool {
		FakeContext context{.load_results = {ParseErrorResult(kConfiguredConfig)}};
		{
			howdy::pam::RuntimeSession session(kConfiguredConfig, kConfiguredModels,
			                                   Dependencies(&context));
			const auto                 result = session.LoadForUser("alice");
			if (!expect(result.status == howdy::pam::RuntimeSessionLoadStatus::kConfigLoadFailed,
			            "direct parse failure returns config-load failure") ||
			    !expect(context.prepare_usernames.empty(),
			            "direct parse failure does not prepare")) {
				return false;
			}
		}
		return true;
	}

	auto TestRootEaccesFailure() -> bool {
		FakeContext context{
		    .load_results  = {PathErrorResult(EACCES)},
		    .effective_uid = 0,
		};
		howdy::pam::RuntimeSession session(kConfiguredConfig, kConfiguredModels,
		                                   Dependencies(&context));
		const auto                 result = session.LoadForUser("alice");
		return expect(result.status == howdy::pam::RuntimeSessionLoadStatus::kConfigLoadFailed,
		              "root EACCES returns config-load failure") &&
		       expect(context.effective_uid_calls == 1, "root EACCES checks effective UID") &&
		       expect(context.prepare_usernames.empty(), "root EACCES does not prepare");
	}

	auto TestNonRootNonEaccesFailure() -> bool {
		FakeContext context{
		    .load_results  = {PathErrorResult(ENOENT)},
		    .effective_uid = 1000,
		};
		howdy::pam::RuntimeSession session(kConfiguredConfig, kConfiguredModels,
		                                   Dependencies(&context));
		const auto                 result = session.LoadForUser("alice");
		return expect(result.status == howdy::pam::RuntimeSessionLoadStatus::kConfigLoadFailed,
		              "non-root non-EACCES returns config-load failure") &&
		       expect(context.effective_uid_calls == 0, "non-EACCES does not need effective UID") &&
		       expect(context.prepare_usernames.empty(), "non-root non-EACCES does not prepare");
	}

	auto TestStagedSuccess() -> bool {
		const auto  staged_runtime = MakePreparedRuntime("stage1");
		FakeContext context{
		    .load_results  = {PathErrorResult(EACCES), SuccessResult(staged_runtime.config_path)},
		    .effective_uid = 1000,
		};
		bool ok = true;
		{
			howdy::pam::RuntimeSession session(kConfiguredConfig, kConfiguredModels,
			                                   Dependencies(&context));
			const auto                 result = session.LoadForUser("alice");
			ok &= expect(result.Ok(), "staged config success returns ok");
			ok &= expect(context.load_paths ==
			                 std::vector<std::filesystem::path>{kConfiguredConfig,
			                                                    staged_runtime.config_path},
			             "staged success loads configured then staged path");
			ok &= expect(context.prepare_usernames == std::vector<std::string>{"alice"},
			             "staged success prepares once for alice");
			ok &= expect(session.Staged(), "staged success reports staged");
			ok &= expect(session.ConfigPath() == staged_runtime.config_path,
			             "staged success uses staged config");
			ok &= expect(session.UserModelsDir() == staged_runtime.user_models_dir,
			             "staged success uses staged models");
		}
		ok &= expect(IssuedLeaseHasState(context, false),
		             "staged success closes lease at destruction");
		return ok;
	}

	auto TestPrepareFailure() -> bool {
		FakeContext context{
		    .load_results     = {PathErrorResult(EACCES)},
		    .prepare_succeeds = false,
		    .effective_uid    = 1000,
		};
		{
			howdy::pam::RuntimeSession session(kConfiguredConfig, kConfiguredModels,
			                                   Dependencies(&context));
			const auto                 result = session.LoadForUser("alice");
			if (!expect(result.status == howdy::pam::RuntimeSessionLoadStatus::kPrepareFailed,
			            "prepare failure returns prepare-failed") ||
			    !expect(context.load_paths.size() == 1,
			            "prepare failure does not load staged config") ||
			    !expect(context.prepare_usernames == std::vector<std::string>{"alice"},
			            "prepare failure calls prepare once")) {
				return false;
			}
		}
		return true;
	}

	auto TestStagedConfigFailure() -> bool {
		const auto  staged_runtime = MakePreparedRuntime("stage1");
		FakeContext context{
		    .load_results = {PathErrorResult(EACCES), ParseErrorResult(staged_runtime.config_path)},
		    .effective_uid = 1000,
		};
		bool ok = true;
		{
			howdy::pam::RuntimeSession session(kConfiguredConfig, kConfiguredModels,
			                                   Dependencies(&context));
			const auto                 result = session.LoadForUser("alice");
			ok &= expect(result.status == howdy::pam::RuntimeSessionLoadStatus::kConfigLoadFailed,
			             "staged parse failure returns config-load failure");
			ok &= expect(IssuedLeaseHasState(context, true),
			             "staged parse failure retains lease until destruction");
		}
		ok &= expect(IssuedLeaseHasState(context, false),
		             "staged parse failure closes lease at destruction");
		return ok;
	}

	auto TestMissingDependencies() -> bool {
		bool ok = true;
		for (int missing = 0; missing < 3; ++missing) {
			FakeContext context;
			auto        deps = Dependencies(&context);
			switch (missing) {
				case 0:
					deps.prepare_runtime = nullptr;
					break;
				case 1:
					deps.load_runtime_config = nullptr;
					break;
				case 2:
					deps.effective_uid = nullptr;
					break;
				default:
					break;
			}

			{
				howdy::pam::RuntimeSession session(kConfiguredConfig, kConfiguredModels, deps);
				const auto                 result = session.LoadForUser("alice");
				ok &= expect(result.status ==
				                 howdy::pam::RuntimeSessionLoadStatus::kInvalidDependencies,
				             "missing dependency returns invalid-dependencies");
			}
			ok &= expect(NoCallbacksRan(context), "missing dependency invokes no callbacks");
		}
		return ok;
	}

	auto TestNoDuplicateCleanup() -> bool {
		const auto  staged_runtime = MakePreparedRuntime("stage1");
		FakeContext context{
		    .load_results  = {PathErrorResult(EACCES), SuccessResult(staged_runtime.config_path)},
		    .effective_uid = 1000,
		};
		{
			howdy::pam::RuntimeSession session(kConfiguredConfig, kConfiguredModels,
			                                   Dependencies(&context));
			if (!expect(session.LoadForUser("alice").Ok(),
			            "duplicate-cleanup setup stages successfully")) {
				return false;
			}
		}
		return expect(IssuedLeaseHasState(context, false), "scope exit closes staged lease");
	}

	auto TestDirectSuccessIsOneShot() -> bool {
		FakeContext context{.load_results = {SuccessResult(kConfiguredConfig)}};
		bool        ok = true;
		{
			howdy::pam::RuntimeSession session(kConfiguredConfig, kConfiguredModels,
			                                   Dependencies(&context));
			const auto                 first = session.LoadForUser("alice");
			ok &= expect(first.status == howdy::pam::RuntimeSessionLoadStatus::kOk,
			             "direct one-shot first load succeeds");
			const auto counts_after_first = GetCallbackCounts(context);

			const auto second = session.LoadForUser("bob");
			ok &= expect(second.status == howdy::pam::RuntimeSessionLoadStatus::kAlreadyLoaded,
			             "direct one-shot rejects second load");
			ok &= expect(!second.Ok(), "already-loaded direct result is not ok");
			ok &= expect(GetCallbackCounts(context) == counts_after_first,
			             "direct re-entry invokes no callbacks");
			ok &= expect(session.ConfigPath() == kConfiguredConfig,
			             "direct re-entry preserves configured config path");
			ok &= expect(session.UserModelsDir() == kConfiguredModels,
			             "direct re-entry preserves configured models path");
		}
		return ok;
	}

	auto TestStagedSuccessIsOneShot() -> bool {
		const auto  runtime = MakePreparedRuntime("run001");
		FakeContext context{
		    .load_results  = {PathErrorResult(EACCES), SuccessResult(runtime.config_path)},
		    .effective_uid = 1000,
		    .prepared      = runtime,
		};
		bool ok = true;
		{
			howdy::pam::RuntimeSession session(kConfiguredConfig, kConfiguredModels,
			                                   Dependencies(&context));
			const auto                 first = session.LoadForUser("alice");
			ok &= expect(first.status == howdy::pam::RuntimeSessionLoadStatus::kOk,
			             "staged one-shot first load succeeds");
			ok &= expect(session.ConfigPath() == runtime.config_path,
			             "staged one-shot activates runtime-a config");
			ok &= expect(session.UserModelsDir() == runtime.user_models_dir,
			             "staged one-shot activates runtime-a models");
			const auto counts_after_first = GetCallbackCounts(context);

			const auto second = session.LoadForUser("bob");
			ok &= expect(second.status == howdy::pam::RuntimeSessionLoadStatus::kAlreadyLoaded,
			             "staged one-shot rejects second load");
			ok &= expect(GetCallbackCounts(context) == counts_after_first,
			             "staged re-entry invokes no callbacks");
			ok &= expect(session.ConfigPath() == runtime.config_path,
			             "staged re-entry preserves runtime-a config");
			ok &= expect(session.UserModelsDir() == runtime.user_models_dir,
			             "staged re-entry preserves runtime-a models");
			ok &= expect(IssuedLeaseHasState(context, true),
			             "staged re-entry retains original lease");
		}
		ok &= expect(IssuedLeaseHasState(context, false),
		             "staged re-entry closes original lease at destruction");
		return ok;
	}

	auto TestFailedStagedLoadIsOneShot() -> bool {
		const auto  runtime = MakePreparedRuntime("fail01");
		FakeContext context{
		    .load_results  = {PathErrorResult(EACCES), ParseErrorResult(runtime.config_path)},
		    .effective_uid = 1000,
		    .prepared      = runtime,
		};
		bool ok = true;
		{
			howdy::pam::RuntimeSession session(kConfiguredConfig, kConfiguredModels,
			                                   Dependencies(&context));
			const auto                 first = session.LoadForUser("alice");
			ok &= expect(first.status == howdy::pam::RuntimeSessionLoadStatus::kConfigLoadFailed,
			             "failed staged one-shot reports config failure");
			const auto counts_after_first = GetCallbackCounts(context);

			const auto second = session.LoadForUser("bob");
			ok &= expect(second.status == howdy::pam::RuntimeSessionLoadStatus::kAlreadyLoaded,
			             "failed staged one-shot rejects second load");
			ok &= expect(GetCallbackCounts(context) == counts_after_first,
			             "failed staged re-entry invokes no callbacks");
			ok &= expect(IssuedLeaseHasState(context, true),
			             "failed staged re-entry retains original lease");
		}
		ok &= expect(IssuedLeaseHasState(context, false),
		             "failed staged re-entry closes original lease at destruction");
		return ok;
	}

	auto TestInvalidDependenciesAreOneShot() -> bool {
		FakeContext context;
		auto        deps     = Dependencies(&context);
		deps.prepare_runtime = nullptr;
		bool ok              = true;
		{
			howdy::pam::RuntimeSession session(kConfiguredConfig, kConfiguredModels, deps);
			const auto                 first = session.LoadForUser("alice");
			ok &= expect(first.status == howdy::pam::RuntimeSessionLoadStatus::kInvalidDependencies,
			             "invalid dependency first load fails validation");
			ok &= expect(NoCallbacksRan(context),
			             "invalid dependency first load invokes no callbacks");

			const auto second = session.LoadForUser("bob");
			ok &= expect(second.status == howdy::pam::RuntimeSessionLoadStatus::kAlreadyLoaded,
			             "invalid dependency session rejects second load");
			ok &=
			    expect(NoCallbacksRan(context), "invalid dependency re-entry invokes no callbacks");
		}
		return ok;
	}

	auto TestInvalidPreparedRuntimeFiles() -> bool {
		namespace fs = std::filesystem;
		using howdy::native::auth_helper_protocol::PreparedUserModelsDir;

		struct TestCase {
			std::string_view                 name;
			howdy::pam::PreparedRuntimeFiles prepared;
		};

		const auto valid   = MakePreparedRuntime("valid1");
		const auto sibling = MakePreparedRuntime("sibling");

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
		sibling_models.user_models_dir = PreparedUserModelsDir(sibling.root_dir).string();
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
		    {.name = "empty config path", .prepared = empty_config},
		    {.name = "empty models path", .prepared = empty_models},
		    {.name = "empty root path", .prepared = empty_root},
		    {.name = "config path outside prepared runtime root", .prepared = config_outside},
		    {.name     = "user-model directory outside prepared runtime root",
		     .prepared = models_outside},
		    {.name = "sibling path with valid-looking basename", .prepared = sibling_models},
		    {.name = "invalid prepared runtime suffix", .prepared = sibling},
		    {.name = "config and models paths swapped", .prepared = swapped_paths},
		    {.name = "unexpected nested config path", .prepared = nested_config},
		    {.name = "models path with parent traversal", .prepared = escaped_models},
		    {.name = "equivalent noncanonical runtime root", .prepared = noncanonical_root},
		    {.name = "unexpected config basename", .prepared = wrong_config_name},
		    {.name = "unexpected models basename", .prepared = wrong_models_name},
		    {.name = "relative prepared paths", .prepared = relative_paths},
		    {.name = "runtime root outside /run/howdy", .prepared = outside_root},
		};

		bool ok = true;
		for (const auto &test_case : test_cases) {
			FakeContext context{
			    .load_results  = {PathErrorResult(EACCES)},
			    .effective_uid = 1000,
			    .prepared      = test_case.prepared,
			};
			{
				howdy::pam::RuntimeSession session(kConfiguredConfig, kConfiguredModels,
				                                   Dependencies(&context));
				const auto                 result = session.LoadForUser("alice");
				const std::string          name(test_case.name);
				ok &= expect(result.status == howdy::pam::RuntimeSessionLoadStatus::kPrepareFailed,
				             name + " is rejected");
				ok &= expect(context.load_paths.size() == 1, name + " does not load staged config");
				ok &= expect(!session.Staged(), name + " is not staged");
				ok &= expect(session.ConfigPath() == kConfiguredConfig,
				             name + " preserves configured config path");
				ok &= expect(session.UserModelsDir() == kConfiguredModels,
				             name + " preserves configured models path");
				ok &= expect(IssuedLeaseHasState(context, false),
				             name + " closes rejected lease immediately");
			}
			ok &= expect(IssuedLeaseHasState(context, false),
			             std::string(test_case.name) + " keeps rejected lease closed");
		}
		return ok;
	}

}  // namespace

auto main(int argc, char **argv) -> int {
	if (argc == 3 && std::string_view(argv[1]) == "--fd3-probe") {
		return RunRuntimeSessionFd3Probe(argv[2]);
	}

	bool ok = true;
	ok &= TestDirectSuccess();
	ok &= TestDirectParseFailure();
	ok &= TestRootEaccesFailure();
	ok &= TestNonRootNonEaccesFailure();
	ok &= TestStagedSuccess();
	ok &= TestPrepareFailure();
	ok &= TestStagedConfigFailure();
	ok &= TestMissingDependencies();
	ok &= TestNoDuplicateCleanup();
	ok &= TestDirectSuccessIsOneShot();
	ok &= TestStagedSuccessIsOneShot();
	ok &= TestFailedStagedLoadIsOneShot();
	ok &= TestInvalidDependenciesAreOneShot();
	ok &= RunRuntimeSessionSpawnTests();
	ok &= RunRuntimeSessionDeadlineTests();
	ok &= TestInvalidPreparedRuntimeFiles();
	return ok ? 0 : 1;
}
