#pragma once

// Store contracts shared by user_model_store.cpp, user_models.cpp, transaction.cpp, and
// mutations.cpp: document loading and the Mutate transaction, which depend on the codec. Never
// include this from readiness.cpp (see src/storage/user_model_store/AGENTS.md).

#include "../user_model_codec/internal.hpp"
#include "readiness/internal.hpp"
#include "storage/user_model_status.hpp"
#include "storage/user_model_store.hpp"
#include "storage/user_model_types.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace howdy::native::user_model_store_internal {

	[[nodiscard]] auto InspectRegularFileStatus(const std::filesystem::path &path,
	                                            std::string *message) -> UserModelStatus;

	struct UserModelExpectations {
		std::string backend;
		std::string model;
		bool        strict_shape = false;
	};

	enum class MutationExistence : std::uint8_t {
		kAllowMissing,
		kRequireExisting,
	};

	struct MutationOptions {
		MutationExistence existence     = MutationExistence::kRequireExisting;
		bool              load_document = true;
	};

	enum class MutationAction : std::uint8_t {
		kAbort,
		kWrite,
		kRemove,
	};

	struct MutationDecision {
		// A decision that names no outcome aborts as a failure, never as success.
		MutationAction  action = MutationAction::kAbort;
		UserModelStatus status = UserModelStatus::kParseError;
		std::string     error_message;
		std::string     failure_message;
		std::string     sync_failed_message;
		UserModelEntry  entry;
	};

	struct MutationContext {
		user_model_codec::Document           document;
		std::optional<UserModelFileSnapshot> snapshot;
	};

	using MutationDecider = std::function<MutationDecision(MutationContext &)>;

	// Validates an already-open descriptor and decodes its document.
	[[nodiscard]] auto LoadDocumentFromFd(int fd, const std::filesystem::path &path,
	                                      const UserModelExpectations &expectations,
	                                      uid_t owner_uid) -> user_model_codec::Document;

	// Turns a loaded document into the flat encoding list compare matches against.
	[[nodiscard]] auto FlattenLoadedModels(user_model_codec::Document &document)
	    -> UserModelLoadResult;

	// Runs the cohesive namespace-lock -> open -> revalidate -> decide -> commit/rollback
	// transaction.
	[[nodiscard]] auto Mutate(const UserModelStoreConfig &config, const std::string &user,
	                          const MutationOptions &options, const MutationDecider &decide)
	    -> UserModelMutationResult;

	[[nodiscard]] auto AppendEntry(const UserModelStoreConfig &config, const std::string &user,
	                               const NewUserModelEntry &new_entry) -> UserModelMutationResult;

	[[nodiscard]] auto RemoveEntryIfMatches(const UserModelStoreConfig      &config,
	                                        const std::string               &user,
	                                        const UserModelEntryExpectation &expected)
	    -> UserModelMutationResult;

	[[nodiscard]] auto ClearEntriesIfUnchanged(const UserModelStoreConfig  &config,
	                                           const std::string           &user,
	                                           const UserModelFileSnapshot &expected_snapshot)
	    -> UserModelMutationResult;

}  // namespace howdy::native::user_model_store_internal
