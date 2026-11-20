#pragma once

#include "config/runtime_config_loader.hpp"
#include "protocol/runtime_descriptor_message.hpp"
#include "support/scoped_fd.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <sys/types.h>

namespace howdy::pam {

	// The runtime config and the user's model, as descriptors opened from the root-owned store,
	// either in this process (root) or by the setuid auth helper's `open`.
	struct RuntimeDescriptorSet {
		howdy::native::ScopedFd                               config;
		howdy::native::runtime_descriptor_message::ModelState model_state =
		    howdy::native::runtime_descriptor_message::ModelState::kInvalidStorage;
		// Open exactly when model_state is kReady.
		howdy::native::ScopedFd model;
		// Why the model is not ready, when the opener said; the helper logs its own reasons.
		std::string model_message;
	};

	struct RuntimeSessionPaths {
		std::string config_path;
		std::string user_models_dir;
	};

	using OpenRuntimeInProcessFn = bool (*)(void *context, std::string_view username,
	                                        const RuntimeSessionPaths &paths,
	                                        RuntimeDescriptorSet      *descriptors);

	using OpenRuntimeThroughHelperFn = bool (*)(void *context, std::string_view username,
	                                            RuntimeDescriptorSet *descriptors);

	using LoadRuntimeConfigFn = howdy::native::RuntimeConfigLoadResult (*)(
	    void *context, int config_fd, const std::filesystem::path &display_path, uid_t owner_uid);

	using EffectiveUidFn = uid_t (*)(void *context);

	class RuntimeSessionOperations {
	public:
		static auto Create(void *context, OpenRuntimeInProcessFn open_in_process,
		                   OpenRuntimeThroughHelperFn open_through_helper,
		                   LoadRuntimeConfigFn load_runtime_config, EffectiveUidFn effective_uid)
		    -> std::optional<RuntimeSessionOperations> {
			if (open_in_process == nullptr || open_through_helper == nullptr ||
			    load_runtime_config == nullptr || effective_uid == nullptr) {
				return std::nullopt;
			}
			return RuntimeSessionOperations(context, open_in_process, open_through_helper,
			                                load_runtime_config, effective_uid);
		}

		[[nodiscard]] auto OpenInProcess(std::string_view           username,
		                                 const RuntimeSessionPaths &paths,
		                                 RuntimeDescriptorSet      *descriptors) const -> bool {
			return open_in_process_(context_, username, paths, descriptors);
		}

		[[nodiscard]] auto OpenThroughHelper(std::string_view      username,
		                                     RuntimeDescriptorSet *descriptors) const -> bool {
			return open_through_helper_(context_, username, descriptors);
		}

		[[nodiscard]] auto LoadRuntimeConfig(int                          config_fd,
		                                     const std::filesystem::path &display_path,
		                                     uid_t                        owner_uid) const
		    -> howdy::native::RuntimeConfigLoadResult {
			return load_runtime_config_(context_, config_fd, display_path, owner_uid);
		}

		[[nodiscard]] auto EffectiveUid() const -> uid_t {
			return effective_uid_(context_);
		}

	private:
		RuntimeSessionOperations(void *context, OpenRuntimeInProcessFn open_in_process,
		                         OpenRuntimeThroughHelperFn open_through_helper,
		                         LoadRuntimeConfigFn        load_runtime_config,
		                         EffectiveUidFn             effective_uid)
		    : context_(context)
		    , open_in_process_(open_in_process)
		    , open_through_helper_(open_through_helper)
		    , load_runtime_config_(load_runtime_config)
		    , effective_uid_(effective_uid) {}

		void                      *context_             = nullptr;
		OpenRuntimeInProcessFn     open_in_process_     = nullptr;
		OpenRuntimeThroughHelperFn open_through_helper_ = nullptr;
		LoadRuntimeConfigFn        load_runtime_config_ = nullptr;
		EffectiveUidFn             effective_uid_       = nullptr;
	};

	enum class RuntimeSessionLoadStatus : std::uint8_t {
		kOk,
		// The descriptors could not be opened, or what arrived failed validation.
		kOpenFailed,
		kConfigLoadFailed,
		kAlreadyLoaded,
	};

	struct RuntimeSessionLoadResult {
		RuntimeSessionLoadStatus               status = RuntimeSessionLoadStatus::kConfigLoadFailed;
		howdy::native::RuntimeConfigLoadResult config_result;
		// Why the descriptors were rejected (kOpenFailed), when the session itself knows.
		std::string error_message;
	};

	[[nodiscard]] inline auto RuntimeSessionLoadSucceeded(const RuntimeSessionLoadResult &result)
	    -> bool {
		return result.status == RuntimeSessionLoadStatus::kOk &&
		       result.config_result.status == howdy::native::RuntimeConfigLoadStatus::kOk &&
		       result.config_result.config.has_value();
	}

	// One-shot. A root PAM module opens the config and model itself; any other caller asks the
	// setuid helper, which can only open them for the calling user. Either way the descriptors
	// are validated here and owned until the session ends, so they stay open for compare's spawn.
	class RuntimeSession {
	public:
		RuntimeSession(RuntimeSessionPaths paths, uid_t trusted_owner_uid,
		               RuntimeSessionOperations operations);

		RuntimeSession(const RuntimeSession &)                     = delete;
		auto operator=(const RuntimeSession &) -> RuntimeSession & = delete;
		RuntimeSession(RuntimeSession &&)                          = delete;
		auto operator=(RuntimeSession &&) -> RuntimeSession &      = delete;

		~RuntimeSession();

		auto LoadForUser(std::string_view username) -> RuntimeSessionLoadResult;

		[[nodiscard]] auto ModelState() const
		    -> howdy::native::runtime_descriptor_message::ModelState;
		[[nodiscard]] auto ModelMessage() const -> const std::string &;
		// Borrowed; -1 until a load succeeds (and, for the model, unless it is ready).
		[[nodiscard]] auto ConfigFd() const -> int;
		[[nodiscard]] auto ModelFd() const -> int;

	private:
		RuntimeSessionPaths      paths_;
		uid_t                    trusted_owner_uid_;
		RuntimeSessionOperations operations_;
		RuntimeDescriptorSet     descriptors_;
		bool                     load_started_ = false;
	};

	auto ProductionRuntimeSessionOperations() -> RuntimeSessionOperations;

}  // namespace howdy::pam
