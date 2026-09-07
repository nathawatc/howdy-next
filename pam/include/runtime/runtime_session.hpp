#pragma once

#include "config/runtime_config.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include <sys/types.h>

namespace howdy::pam {

	struct PreparedRuntimeFiles {
		std::filesystem::path root_dir;
		std::string           config_path;
		std::string           user_models_dir;
		int                   lease_fd = -1;
	};

	using PrepareRuntimeFilesFn = bool (*)(void *context, std::string_view username,
	                                       PreparedRuntimeFiles *prepared);

	using LoadRuntimeConfigFn = howdy::native::RuntimeConfigLoadResult (*)(
	    void *context, const std::filesystem::path &config_path);

	using EffectiveUidFn = uid_t (*)(void *context);

	struct RuntimeSessionDependencies {
		void                 *context             = nullptr;
		PrepareRuntimeFilesFn prepare_runtime     = nullptr;
		LoadRuntimeConfigFn   load_runtime_config = nullptr;
		EffectiveUidFn        effective_uid       = nullptr;
	};

	enum class RuntimeSessionLoadStatus : std::uint8_t {
		kOk,
		kPrepareFailed,
		kConfigLoadFailed,
		kInvalidDependencies,
		kAlreadyLoaded,
	};

	struct RuntimeSessionLoadResult {
		RuntimeSessionLoadStatus status = RuntimeSessionLoadStatus::kInvalidDependencies;
		howdy::native::RuntimeConfigLoadResult config_result;

		[[nodiscard]] auto Ok() const -> bool {
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

		auto LoadForUser(std::string_view username) -> RuntimeSessionLoadResult;

		[[nodiscard]] auto ConfigPath() const -> const std::string &;
		[[nodiscard]] auto UserModelsDir() const -> const std::string &;
		[[nodiscard]] auto Staged() const -> bool;

	private:
		std::string                config_path_;
		std::string                user_models_dir_;
		RuntimeSessionDependencies dependencies_;
		int                        lease_fd_     = -1;
		bool                       load_started_ = false;
	};

	auto ProductionRuntimeSessionDependencies() -> RuntimeSessionDependencies;

}  // namespace howdy::pam
