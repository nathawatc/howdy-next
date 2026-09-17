#include "runtime/runtime_session.hpp"

#include "protocol/auth_helper_protocol.hpp"
#include "runtime/auth_helper_process.hpp"

#include <cerrno>
#include <stdexcept>
#include <unistd.h>
#include <utility>

namespace {
	auto PreparedRuntimeFilesMatchContract(const howdy::pam::PreparedRuntimeFiles &prepared)
	    -> bool {
		return howdy::native::auth_helper_protocol::MatchesPreparedRuntimeLayout(
		    prepared.root_dir, std::filesystem::path(prepared.config_path),
		    std::filesystem::path(prepared.user_models_dir), getuid());
	}

	auto PrepareRuntimeFilesDependency(void *context, std::string_view username,
	                                   howdy::pam::PreparedRuntimeFiles *prepared) -> bool {
		(void)context;
		return howdy::pam::auth_helper_process::PrepareRuntimeAuthFiles(username, prepared);
	}

	auto LoadRuntimeConfigDependency(void *context, const std::filesystem::path &config_path)
	    -> howdy::native::RuntimeConfigLoadResult {
		(void)context;
		return howdy::native::LoadRuntimeConfig(config_path, static_cast<uid_t>(0));
	}

	auto EffectiveUidDependency(void *context) -> uid_t {
		(void)context;
		return geteuid();
	}
}  // namespace

namespace howdy::pam {

	RuntimeSession::RuntimeSession(std::string              configured_config_path,
	                               std::string              configured_user_models_dir,
	                               RuntimeSessionOperations operations)
	    : config_path_(std::move(configured_config_path))
	    , user_models_dir_(std::move(configured_user_models_dir))
	    , operations_(operations) {}

	RuntimeSession::~RuntimeSession() {
		if (lease_fd_ >= 0) {
			(void)close(lease_fd_);
			lease_fd_ = -1;
		}
	}

	auto RuntimeSession::LoadForUser(std::string_view username) -> RuntimeSessionLoadResult {
		if (load_started_) {
			return {
			    .status = RuntimeSessionLoadStatus::kAlreadyLoaded,
			};
		}
		load_started_ = true;

		auto       config_result = operations_.LoadRuntimeConfig(config_path_);
		const bool needs_staging =
		    config_result.status == howdy::native::RuntimeConfigLoadStatus::kPathError &&
		    config_result.error_code == EACCES && operations_.EffectiveUid() != 0;
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
		if (!operations_.PrepareRuntime(username, &prepared)) {
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
		    !PreparedRuntimeFilesMatchContract(prepared)) {
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

		config_result = operations_.LoadRuntimeConfig(config_path_);
		const bool config_ok =
		    config_result.status == howdy::native::RuntimeConfigLoadStatus::kOk &&
		    config_result.config.has_value();
		return RuntimeSessionLoadResult{
		    .status        = config_ok ? RuntimeSessionLoadStatus::kOk
		                               : RuntimeSessionLoadStatus::kConfigLoadFailed,
		    .config_result = std::move(config_result),
		};
	}

	auto RuntimeSession::ConfigPath() const -> const std::string & {
		return config_path_;
	}

	auto RuntimeSession::UserModelsDir() const -> const std::string & {
		return user_models_dir_;
	}

	auto RuntimeSession::Staged() const -> bool {
		return lease_fd_ >= 0;
	}

	auto ProductionRuntimeSessionOperations() -> RuntimeSessionOperations {
		auto operations =
		    RuntimeSessionOperations::Create(nullptr, PrepareRuntimeFilesDependency,
		                                     LoadRuntimeConfigDependency, EffectiveUidDependency);
		if (!operations.has_value()) {
			throw std::logic_error("Failed to create production runtime session operations");
		}
		return *operations;
	}

}  // namespace howdy::pam
