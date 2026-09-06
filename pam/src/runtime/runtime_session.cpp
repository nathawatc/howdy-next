#include "runtime/runtime_session.hpp"

#include "protocol/auth_helper_protocol.hpp"
#include "runtime/auth_helper_process.hpp"

#include <cerrno>
#include <unistd.h>
#include <utility>

namespace {
	auto prepared_runtime_files_match_contract(const howdy::pam::PreparedRuntimeFiles &prepared)
	    -> bool {
		return howdy::native::auth_helper_protocol::matches_prepared_runtime_layout(
		    prepared.root_dir, std::filesystem::path(prepared.config_path),
		    std::filesystem::path(prepared.user_models_dir), getuid());
	}

	auto prepare_runtime_files_dependency(void *context, std::string_view username,
	                                      howdy::pam::PreparedRuntimeFiles *prepared) -> bool {
		(void)context;
		return howdy::pam::auth_helper_process::prepare_runtime_auth_files(username, prepared);
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

	RuntimeSession::RuntimeSession(std::string                configured_config_path,
	                               std::string                configured_user_models_dir,
	                               RuntimeSessionDependencies dependencies)
	    : config_path_(std::move(configured_config_path))
	    , user_models_dir_(std::move(configured_user_models_dir))
	    , dependencies_(dependencies) {}

	RuntimeSession::~RuntimeSession() {
		if (lease_fd_ >= 0) {
			(void)close(lease_fd_);
			lease_fd_ = -1;
		}
	}

	auto RuntimeSession::load_for_user(std::string_view username) -> RuntimeSessionLoadResult {
		if (load_started_) {
			return {
			    .status = RuntimeSessionLoadStatus::kAlreadyLoaded,
			};
		}
		load_started_ = true;

		if (dependencies_.prepare_runtime == nullptr ||
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
			if (prepared.lease_fd >= 0) {
				(void)close(prepared.lease_fd);
			}
			return RuntimeSessionLoadResult{
			    .status        = RuntimeSessionLoadStatus::kPrepareFailed,
			    .config_result = std::move(config_result),
			};
		}
		if (prepared.config_path.empty() || prepared.user_models_dir.empty() ||
		    prepared.root_dir.empty() || prepared.lease_fd < 0 ||
		    !prepared_runtime_files_match_contract(prepared)) {
			if (prepared.lease_fd >= 0) {
				(void)close(prepared.lease_fd);
			}
			return RuntimeSessionLoadResult{
			    .status        = RuntimeSessionLoadStatus::kPrepareFailed,
			    .config_result = std::move(config_result),
			};
		}

		config_path_      = std::move(prepared.config_path);
		user_models_dir_  = std::move(prepared.user_models_dir);
		lease_fd_         = prepared.lease_fd;
		prepared.lease_fd = -1;

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
		return lease_fd_ >= 0;
	}

	auto production_runtime_session_dependencies() -> RuntimeSessionDependencies {
		return RuntimeSessionDependencies{
		    .prepare_runtime     = prepare_runtime_files_dependency,
		    .load_runtime_config = load_runtime_config_dependency,
		    .effective_uid       = effective_uid_dependency,
		};
	}

}  // namespace howdy::pam
