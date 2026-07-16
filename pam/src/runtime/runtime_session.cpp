#include "runtime/runtime_session.hpp"

#include "runtime/auth_helper_process.hpp"

#include <cerrno>
#include <exception>
#include <syslog.h>
#include <unistd.h>
#include <utility>

namespace {
	auto prepare_runtime_files_dependency(void *context, std::string_view username,
	                                      howdy::pam::PreparedRuntimeFiles *prepared) -> bool {
		(void)context;
		return howdy::pam::auth_helper_process::prepare_runtime_auth_files(username, prepared);
	}

	auto cleanup_runtime_files_dependency(void *context, const std::filesystem::path &root_dir)
	    -> void {
		(void)context;
		howdy::pam::auth_helper_process::cleanup_runtime_auth_files(root_dir);
	}

	auto load_runtime_config_dependency(void *context, const std::filesystem::path &config_path)
	    -> howdy::native::RuntimeConfigLoadResult {
		(void)context;
		return howdy::native::load_runtime_config(config_path, static_cast<uid_t>(0));
	}

	auto effective_uid_dependency(void *context) -> uid_t {
		(void)context;
		return geteuid();
	}
}  // namespace

namespace howdy::pam {
	namespace {

		auto invoke_cleanup(const RuntimeSessionDependencies &dependencies,
		                    const std::filesystem::path      &runtime_root) noexcept -> void {
			try {
				dependencies.cleanup_runtime(dependencies.context, runtime_root);
			} catch (const std::exception &error) {
				syslog(LOG_WARNING, "Howdy auth helper cleanup failed: %s", error.what());
			} catch (...) {
				syslog(LOG_WARNING, "Howdy auth helper cleanup failed with non-standard exception");
			}
		}

	}  // namespace

	RuntimeSession::RuntimeSession(std::string                configured_config_path,
	                               std::string                configured_user_models_dir,
	                               RuntimeSessionDependencies dependencies)
	    : config_path_(std::move(configured_config_path))
	    , user_models_dir_(std::move(configured_user_models_dir))
	    , dependencies_(dependencies) {}

	RuntimeSession::~RuntimeSession() {
		if (!cleanup_active_ || runtime_root_.empty()) {
			return;
		}

		cleanup_active_ = false;
		invoke_cleanup(dependencies_, runtime_root_);
	}

	auto RuntimeSession::load_for_user(std::string_view username) -> RuntimeSessionLoadResult {
		if (load_started_) {
			return {
			    .status = RuntimeSessionLoadStatus::kAlreadyLoaded,
			};
		}
		load_started_ = true;

		if (dependencies_.prepare_runtime == nullptr || dependencies_.cleanup_runtime == nullptr ||
		    dependencies_.load_runtime_config == nullptr ||
		    dependencies_.effective_uid == nullptr) {
			return {};
		}

		auto config_result = dependencies_.load_runtime_config(dependencies_.context, config_path_);
		const bool needs_staging =
		    config_result.status == howdy::native::RuntimeConfigLoadStatus::kPathError &&
		    config_result.error_code == EACCES &&
		    dependencies_.effective_uid(dependencies_.context) != 0;
		if (!needs_staging) {
			const bool config_ok =
			    config_result.status == howdy::native::RuntimeConfigLoadStatus::kOk &&
			    config_result.config.has_value();
			return RuntimeSessionLoadResult{
			    .status        = config_ok ? RuntimeSessionLoadStatus::kOk
			                               : RuntimeSessionLoadStatus::kConfigLoadFailed,
			    .config_result = std::move(config_result),
			};
		}

		PreparedRuntimeFiles prepared;
		if (!dependencies_.prepare_runtime(dependencies_.context, username, &prepared)) {
			return RuntimeSessionLoadResult{
			    .status        = RuntimeSessionLoadStatus::kPrepareFailed,
			    .config_result = std::move(config_result),
			};
		}
		if (prepared.config_path.empty() || prepared.user_models_dir.empty() ||
		    prepared.root_dir.empty()) {
			if (!prepared.root_dir.empty()) {
				invoke_cleanup(dependencies_, prepared.root_dir);
			}
			return RuntimeSessionLoadResult{
			    .status        = RuntimeSessionLoadStatus::kPrepareFailed,
			    .config_result = std::move(config_result),
			};
		}

		config_path_     = std::move(prepared.config_path);
		user_models_dir_ = std::move(prepared.user_models_dir);
		runtime_root_    = std::move(prepared.root_dir);
		cleanup_active_  = true;

		config_result = dependencies_.load_runtime_config(dependencies_.context, config_path_);
		const bool config_ok =
		    config_result.status == howdy::native::RuntimeConfigLoadStatus::kOk &&
		    config_result.config.has_value();
		return RuntimeSessionLoadResult{
		    .status        = config_ok ? RuntimeSessionLoadStatus::kOk
		                               : RuntimeSessionLoadStatus::kConfigLoadFailed,
		    .config_result = std::move(config_result),
		};
	}

	auto RuntimeSession::config_path() const -> const std::string & {
		return config_path_;
	}

	auto RuntimeSession::user_models_dir() const -> const std::string & {
		return user_models_dir_;
	}

	auto RuntimeSession::staged() const -> bool {
		return cleanup_active_;
	}

	auto production_runtime_session_dependencies() -> RuntimeSessionDependencies {
		return RuntimeSessionDependencies{
		    .prepare_runtime     = prepare_runtime_files_dependency,
		    .cleanup_runtime     = cleanup_runtime_files_dependency,
		    .load_runtime_config = load_runtime_config_dependency,
		    .effective_uid       = effective_uid_dependency,
		};
	}

}  // namespace howdy::pam
