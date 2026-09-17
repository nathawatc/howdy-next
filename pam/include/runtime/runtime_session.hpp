#pragma once

#include "config/runtime_config_loader.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
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

	class RuntimeSessionOperations {
	public:
		static auto Create(void *context, PrepareRuntimeFilesFn prepare_runtime,
		                   LoadRuntimeConfigFn load_runtime_config, EffectiveUidFn effective_uid)
		    -> std::optional<RuntimeSessionOperations> {
			if (prepare_runtime == nullptr || load_runtime_config == nullptr ||
			    effective_uid == nullptr) {
				return std::nullopt;
			}
			return RuntimeSessionOperations(context, prepare_runtime, load_runtime_config,
			                                effective_uid);
		}

		[[nodiscard]] auto PrepareRuntime(std::string_view      username,
		                                  PreparedRuntimeFiles *prepared) const -> bool {
			return prepare_runtime_(context_, username, prepared);
		}

		[[nodiscard]] auto LoadRuntimeConfig(const std::filesystem::path &config_path) const
		    -> howdy::native::RuntimeConfigLoadResult {
			return load_runtime_config_(context_, config_path);
		}

		[[nodiscard]] auto EffectiveUid() const -> uid_t {
			return effective_uid_(context_);
		}

	private:
		RuntimeSessionOperations(void *context, PrepareRuntimeFilesFn prepare_runtime,
		                         LoadRuntimeConfigFn load_runtime_config,
		                         EffectiveUidFn      effective_uid)
		    : context_(context)
		    , prepare_runtime_(prepare_runtime)
		    , load_runtime_config_(load_runtime_config)
		    , effective_uid_(effective_uid) {}

		void                 *context_             = nullptr;
		PrepareRuntimeFilesFn prepare_runtime_     = nullptr;
		LoadRuntimeConfigFn   load_runtime_config_ = nullptr;
		EffectiveUidFn        effective_uid_       = nullptr;
	};

	enum class RuntimeSessionLoadStatus : std::uint8_t {
		kOk,
		kPrepareFailed,
		kConfigLoadFailed,
		kAlreadyLoaded,
	};

	struct RuntimeSessionLoadResult {
		RuntimeSessionLoadStatus               status = RuntimeSessionLoadStatus::kConfigLoadFailed;
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
		               RuntimeSessionOperations operations);

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
		std::string              config_path_;
		std::string              user_models_dir_;
		RuntimeSessionOperations operations_;
		int                      lease_fd_     = -1;
		bool                     load_started_ = false;
	};

	auto ProductionRuntimeSessionOperations() -> RuntimeSessionOperations;

}  // namespace howdy::pam
