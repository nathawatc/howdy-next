#include "config/runtime_config_loader.hpp"

#include "config/config_utils.hpp"
#include "config/config_validation.hpp"

#include <memory>
#include <utility>

namespace howdy::native {

	auto load_runtime_config(const std::filesystem::path &path,
	                         const std::optional<uid_t>   owner_uid) -> RuntimeConfigLoadResult {
		const auto security = check_secure_config_path(path, owner_uid);
		if (!security.ok) {
			return RuntimeConfigLoadResult{
			    .status        = RuntimeConfigLoadStatus::kPathError,
			    .path          = path,
			    .config        = nullptr,
			    .error_message = security.error_message,
			    .error_code    = security.error_code,
			};
		}

		auto config = std::make_unique<ConfigReader>(path.string());
		if (!config->ok()) {
			return RuntimeConfigLoadResult{
			    .status        = RuntimeConfigLoadStatus::kParseError,
			    .path          = path,
			    .config        = nullptr,
			    .error_message = "Failed to parse config: " + path.string() + " (error " +
			                     std::to_string(config->parse_error()) + ")",
			    .error_code    = 0,
			};
		}

		if (const auto validation = validate_runtime_config(*config)) {
			return RuntimeConfigLoadResult{
			    .status        = RuntimeConfigLoadStatus::kInvalidRuntimeValue,
			    .path          = path,
			    .config        = nullptr,
			    .error_message = "Invalid runtime config in " + path.string() + ": " + *validation,
			    .error_code    = 0,
			};
		}

		return RuntimeConfigLoadResult{
		    .status        = RuntimeConfigLoadStatus::kOk,
		    .path          = path,
		    .config        = std::move(config),
		    .error_message = {},
		    .error_code    = 0,
		};
	}

}  // namespace howdy::native
