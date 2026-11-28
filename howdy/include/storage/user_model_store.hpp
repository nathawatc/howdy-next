#pragma once

#include "storage/user_model_status.hpp"
#include "storage/user_model_store/test_hooks.hpp"
#include "storage/user_model_types.hpp"
#include "support/file_security/validation_root.hpp"
#include "support/scoped_fd.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

#include <sys/types.h>

namespace howdy::native {

	struct UserModelStoreConfig {
		std::filesystem::path                  models_dir;
		uid_t                                  owner_uid = 0;
		file_security_internal::ValidationRoot validation_root;
		user_model_store_test_hooks::Hooks     hooks;
	};

	enum class UserModelDescriptorStatus : std::uint8_t {
		kReady,
		kMissing,
		kInvalidUser,
		kInvalidStorage,
	};

	struct UserModelDescriptorResult {
		UserModelDescriptorStatus status = UserModelDescriptorStatus::kInvalidStorage;
		// Open read-only and close-on-exec exactly when status is kReady.
		ScopedFd    fd;
		std::string error_message;
	};

	class UserModelStore {
	public:
		explicit UserModelStore(UserModelStoreConfig config);

		[[nodiscard]] static auto Production() -> UserModelStore;

		[[nodiscard]] auto OpenRuntimeDescriptor(const std::string &user) const
		    -> UserModelDescriptorResult;
		[[nodiscard]] auto ListEntries(const std::string &user, const std::string &expected_backend,
		                               const std::string &expected_model = {}) const
		    -> UserModelListResult;
		[[nodiscard]] auto InspectFile(const std::string &user) const -> UserModelInspectResult;
		[[nodiscard]] auto LoadModels(const std::string &user, const std::string &expected_backend,
		                              const std::string &expected_model = {}) const
		    -> UserModelLoadResult;
		[[nodiscard]] auto AppendEntry(const std::string       &user,
		                               const NewUserModelEntry &entry) const
		    -> UserModelMutationResult;
		[[nodiscard]] auto RemoveEntryIfMatches(const std::string               &user,
		                                        const UserModelEntryExpectation &expected) const
		    -> UserModelMutationResult;
		[[nodiscard]] auto
		ClearEntriesIfUnchanged(const std::string           &user,
		                        const UserModelFileSnapshot &expected_snapshot) const
		    -> UserModelMutationResult;

	private:
		UserModelStoreConfig config_;
	};

}  // namespace howdy::native
