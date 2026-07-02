#pragma once

#include "config/runtime_config.hpp"

#include <filesystem>
#include <string>
#include <string_view>

#include <sys/types.h>

namespace howdy::pam {

	struct PreparedRuntimeFiles {
		std::filesystem::path root_dir;
		std::string           config_path;
		std::string           user_models_dir;
	};

	using PrepareRuntimeFilesFn = bool (*)(void *context, std::string_view username,
	                                       PreparedRuntimeFiles *prepared);

	using CleanupRuntimeFilesFn = void (*)(void *context, const std::filesystem::path &root_dir);

	using LoadRuntimeConfigFn = howdy::native::RuntimeConfigLoadResult (*)(
	    void *context, const std::filesystem::path &config_path);

	using EffectiveUidFn = uid_t (*)(void *context);

	struct RuntimeSessionDependencies {
		void                 *context             = nullptr;
		PrepareRuntimeFilesFn prepare_runtime     = nullptr;
		CleanupRuntimeFilesFn cleanup_runtime     = nullptr;
		LoadRuntimeConfigFn   load_runtime_config = nullptr;
		EffectiveUidFn        effective_uid       = nullptr;
	};

	enum class RuntimeSessionLoadStatus {
		kOk,
		kPrepareFailed,
		kConfigLoadFailed,
		kInvalidDependencies,
		kAlreadyLoaded,
	};

	struct RuntimeSessionLoadResult {
		RuntimeSessionLoadStatus status = RuntimeSessionLoadStatus::kInvalidDependencies;
		howdy::native::RuntimeConfigLoadResult config_result;

		[[nodiscard]] auto ok() const -> bool {
			return status == RuntimeSessionLoadStatus::kOk &&
			       config_result.status == howdy::native::RuntimeConfigLoadStatus::kOk &&
			       config_result.config.has_value();
		}
	};

	class RuntimeSession {
	public:
		RuntimeSession(std::string configured_config_path, std::string configured_user_models_dir,
		               RuntimeSessionDependencies dependencies);

		RuntimeSession(const RuntimeSession &)                     = delete;
		auto operator=(const RuntimeSession &) -> RuntimeSession & = delete;
		RuntimeSession(RuntimeSession &&)                          = delete;
		auto operator=(RuntimeSession &&) -> RuntimeSession &      = delete;

		~RuntimeSession();

		auto load_for_user(std::string_view username) -> RuntimeSessionLoadResult;

		[[nodiscard]] auto config_path() const -> const std::string &;
		[[nodiscard]] auto user_models_dir() const -> const std::string &;
		[[nodiscard]] auto staged() const -> bool;

	private:
		std::string                config_path_;
		std::string                user_models_dir_;
		RuntimeSessionDependencies dependencies_;
		std::filesystem::path      runtime_root_;
		bool                       cleanup_active_ = false;
		bool                       load_started_   = false;
	};

	auto production_runtime_session_dependencies() -> RuntimeSessionDependencies;

}  // namespace howdy::pam
