#include "storage/user_models.hpp"

#include "common/atomic_files.hpp"
#include "common/file_lock.hpp"
#include "common/file_security.hpp"
#include "common/user_names.hpp"
#include "config/runtime_paths.hpp"
#include "storage/user_model_readiness.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>

#include <sys/stat.h>

#include <nlohmann/json.hpp>

namespace howdy::native {

	namespace {

		constexpr std::uintmax_t kMaxUserModelFileBytes = 1024 * 1024;
		constexpr std::size_t    kMaxStoredModels       = 256;
		constexpr std::size_t    kMaxEncodingsPerModel  = 32;
		constexpr std::size_t    kMaxEncodingLength     = 1024;
		constexpr mode_t kUserModelsDirMode = S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP;
		constexpr mode_t kUserModelFileMode = S_IRUSR | S_IWUSR;

		struct ModelPathResult {
			UserModelStatus       status = UserModelStatus::kOk;
			std::string           error_message;
			std::filesystem::path path;
		};

		struct ModelDocument {
			UserModelListResult result;
			nlohmann::json      models = nlohmann::json::array();
		};

		auto model_changed_failure() -> UserModelMutationResult {
			return UserModelMutationResult{
			    .status        = UserModelStatus::kModelChanged,
			    .error_message = "User model file changed, please rerun the command",
			};
		}

		auto failure(UserModelStatus status, std::string message) -> UserModelListResult {
			return UserModelListResult{
			    .status        = status,
			    .error_message = std::move(message),
			};
		}

		auto mutation_failure(UserModelStatus status, std::string message)
		    -> UserModelMutationResult {
			return UserModelMutationResult{
			    .status        = status,
			    .error_message = std::move(message),
			};
		}

		auto inspect_failure(UserModelStatus status, std::string message)
		    -> UserModelInspectResult {
			return UserModelInspectResult{
			    .status        = status,
			    .error_message = std::move(message),
			};
		}

		auto resolve_model_path(const std::string &user, bool create_directory,
		                        std::optional<uid_t> owner_uid) -> ModelPathResult {
			const auto models_dir = resolve_user_models_dir();
			if (!create_directory) {
				const auto readiness = check_user_model_readiness(models_dir, user, owner_uid);
				return ModelPathResult{
				    .status        = readiness.status,
				    .error_message = readiness.error_message,
				    .path          = readiness.path,
				};
			}

			const auto model_path = resolve_user_model_path(models_dir, user);
			if (!model_path) {
				return ModelPathResult{
				    .status        = UserModelStatus::kInvalidUser,
				    .error_message = kInvalidUserNameMessage,
				};
			}

			std::error_code ec;
			if (!std::filesystem::exists(models_dir, ec)) {
				if (ec) {
					return ModelPathResult{
					    .status = UserModelStatus::kParseError,
					    .error_message =
					        "Failed to inspect user models directory: " + models_dir.string(),
					};
				}
				std::filesystem::create_directories(models_dir, ec);
				if (ec || chmod(models_dir.c_str(), kUserModelsDirMode) != 0) {
					return ModelPathResult{
					    .status = UserModelStatus::kDirectoryCreateFailed,
					    .error_message =
					        "Failed to create secure user models directory: " + models_dir.string(),
					};
				}
			}

			const auto directory_security = check_secure_root_owned_directory_tree(
			    models_dir, "User models directory", owner_uid);
			if (!directory_security.ok) {
				return ModelPathResult{
				    .status        = UserModelStatus::kInsecurePath,
				    .error_message = directory_security.error_message,
				};
			}

			ec.clear();
			const auto model_exists = std::filesystem::exists(*model_path, ec);
			if (ec) {
				return ModelPathResult{
				    .status        = UserModelStatus::kParseError,
				    .error_message = "Failed to inspect user model file: " + model_path->string(),
				};
			}
			if (model_exists) {
				const auto file_security = check_secure_root_owned_file_with_directory(
				    *model_path, "User models directory", "User model file", owner_uid);
				if (!file_security.ok) {
					return ModelPathResult{
					    .status        = UserModelStatus::kInsecurePath,
					    .error_message = file_security.error_message,
					};
				}
			}

			return ModelPathResult{.path = *model_path};
		}

		auto snapshot_model_file(const std::filesystem::path &path)
		    -> std::optional<UserModelFileSnapshot> {
			struct stat st{};
			if (stat(path.c_str(), &st) != 0) {
				return std::nullopt;
			}
			return UserModelFileSnapshot{
			    .dev            = static_cast<std::uint64_t>(st.st_dev),
			    .inode          = static_cast<std::uint64_t>(st.st_ino),
			    .size           = static_cast<std::uintmax_t>(st.st_size),
			    .mtime_seconds  = static_cast<long long>(st.st_mtim.tv_sec),
			    .mtime_nanosecs = static_cast<long long>(st.st_mtim.tv_nsec),
			    .ctime_seconds  = static_cast<long long>(st.st_ctim.tv_sec),
			    .ctime_nanosecs = static_cast<long long>(st.st_ctim.tv_nsec),
			};
		}

		auto snapshots_match(const UserModelFileSnapshot &left, const UserModelFileSnapshot &right)
		    -> bool {
			return left.dev == right.dev && left.inode == right.inode && left.size == right.size &&
			       left.mtime_seconds == right.mtime_seconds &&
			       left.mtime_nanosecs == right.mtime_nanosecs &&
			       left.ctime_seconds == right.ctime_seconds &&
			       left.ctime_nanosecs == right.ctime_nanosecs;
		}

		auto entry_matches(const UserModelEntry &entry, const UserModelEntryExpectation &expected)
		    -> bool {
			return entry.id == expected.id && entry.time == expected.time &&
			       entry.label == expected.label && entry.backend == expected.backend &&
			       entry.metric == expected.metric && entry.model == expected.model;
		}

		auto inspect_regular_file_status(const std::filesystem::path &path, std::string *message)
		    -> UserModelStatus {
			std::error_code regular_ec;
			if (std::filesystem::is_regular_file(path, regular_ec)) {
				return UserModelStatus::kOk;
			}
			if (regular_ec) {
				std::error_code exists_ec;
				const auto      exists = std::filesystem::exists(path, exists_ec);
				if (!exists && !exists_ec) {
					return UserModelStatus::kNoModel;
				}
				*message = "Failed to inspect user model file: " + path.string();
				return UserModelStatus::kParseError;
			}
			return UserModelStatus::kNoModel;
		}

		auto validate_compatibility(const UserModelEntry &entry,
		                            const std::string    &expected_backend,
		                            const std::string    &expected_metric,
		                            const std::string    &expected_model) -> UserModelListResult {
			if (!expected_backend.empty() && !entry.backend.empty() &&
			    entry.backend != expected_backend) {
				return failure(UserModelStatus::kIncompatibleBackend,
				               "Stored face models use incompatible face-recognition metadata");
			}
			if (!expected_metric.empty() && !entry.metric.empty() &&
			    entry.metric != expected_metric) {
				return failure(UserModelStatus::kIncompatibleMetric,
				               "Stored face models use incompatible face-recognition metadata");
			}
			if (!expected_model.empty() && !entry.model.empty() && entry.model != expected_model) {
				return failure(UserModelStatus::kIncompatibleModel,
				               "Stored face models use incompatible face-recognition metadata");
			}
			return UserModelListResult{.status = UserModelStatus::kOk};
		}

		auto read_int_field(const nlohmann::json &model, const char *key, bool strict_shape)
		    -> std::optional<int> {
			const auto value = model.find(key);
			if (value == model.end()) {
				return strict_shape ? std::nullopt : std::optional<int>(-1);
			}
			if (!value->is_number_integer()) {
				return std::nullopt;
			}
			const auto raw = value->get<long long>();
			if (raw < std::numeric_limits<int>::min() || raw > std::numeric_limits<int>::max()) {
				return std::nullopt;
			}
			return static_cast<int>(raw);
		}

		auto read_time_field(const nlohmann::json &model) -> std::optional<long long> {
			const auto value = model.find("time");
			if (value == model.end()) {
				return 0;
			}
			if (!value->is_number_integer()) {
				return std::nullopt;
			}
			return value->get<long long>();
		}

		auto read_string_field(const nlohmann::json &model, const char *key, bool strict_shape)
		    -> std::optional<std::string> {
			const auto value = model.find(key);
			if (value == model.end()) {
				return std::string();
			}
			if (!value->is_string()) {
				return strict_shape ? std::nullopt : std::optional<std::string>(std::string());
			}
			return value->get<std::string>();
		}

		auto validate_encoding_values(const std::vector<float> &encoding) -> UserModelListResult {
			if (encoding.empty() || encoding.size() > kMaxEncodingLength) {
				return failure(UserModelStatus::kOversized,
				               "Stored face encoding exceeds safety limit");
			}
			if (!std::ranges::all_of(encoding, [](float value) {
				    return std::isfinite(value);
			    })) {
				return failure(UserModelStatus::kInvalidShape,
				               "Stored face encoding contains an invalid value");
			}
			return UserModelListResult{.status = UserModelStatus::kOk};
		}

		struct EncodingParseResult {
			UserModelListResult result{.status = UserModelStatus::kOk};
			std::vector<float>  encoding;
		};

		auto parse_encoding(const nlohmann::json &encoding_json) -> EncodingParseResult {
			if (!encoding_json.is_array()) {
				return EncodingParseResult{
				    .result = failure(UserModelStatus::kInvalidShape,
				                      "Stored face encoding is not an array"),
				};
			}
			if (encoding_json.empty() || encoding_json.size() > kMaxEncodingLength) {
				return EncodingParseResult{
				    .result = failure(UserModelStatus::kOversized,
				                      "Stored face encoding exceeds safety limit"),
				};
			}

			std::vector<float> encoding;
			encoding.reserve(encoding_json.size());
			for (const auto &value : encoding_json) {
				if (!value.is_number()) {
					return EncodingParseResult{
					    .result = failure(UserModelStatus::kInvalidShape,
					                      "Stored face encoding contains an invalid value"),
					};
				}
				const auto number = value.get<double>();
				if (!std::isfinite(number) || number < -std::numeric_limits<float>::max() ||
				    number > std::numeric_limits<float>::max()) {
					return EncodingParseResult{
					    .result = failure(UserModelStatus::kInvalidShape,
					                      "Stored face encoding contains an invalid value"),
					};
				}
				encoding.push_back(static_cast<float>(number));
			}

			return EncodingParseResult{.encoding = std::move(encoding)};
		}

		struct EntryParseResult {
			UserModelListResult result{.status = UserModelStatus::kOk};
			UserModelEntry      entry;
		};

		auto parse_model_entry(const nlohmann::json &model, const std::string &expected_backend,
		                       const std::string &expected_metric,
		                       const std::string &expected_model, bool strict_shape)
		    -> EntryParseResult {
			if (!model.is_object()) {
				return EntryParseResult{
				    .result = failure(UserModelStatus::kInvalidShape,
				                      "Model file contains an invalid model entry"),
				};
			}

			const auto id = read_int_field(model, "id", strict_shape);
			if (!id.has_value()) {
				return EntryParseResult{
				    .result = failure(UserModelStatus::kInvalidShape,
				                      "Stored face model ID is not an integer"),
				};
			}
			if (strict_shape && *id < 0) {
				return EntryParseResult{
				    .result = failure(UserModelStatus::kInvalidShape,
				                      "Stored face model ID must not be negative"),
				};
			}

			const auto time = read_time_field(model);
			if (!time.has_value()) {
				return EntryParseResult{
				    .result = failure(UserModelStatus::kInvalidShape,
				                      "Stored face model timestamp is not an integer"),
				};
			}

			const auto label = read_string_field(model, "label", strict_shape);
			if (!label.has_value()) {
				return EntryParseResult{
				    .result = failure(UserModelStatus::kInvalidShape,
				                      "Stored face model label is not a string"),
				};
			}
			const auto backend    = read_string_field(model, "backend", strict_shape);
			const auto metric     = read_string_field(model, "metric", strict_shape);
			const auto model_name = read_string_field(model, "model", strict_shape);
			if (!backend.has_value() || !metric.has_value() || !model_name.has_value()) {
				return EntryParseResult{
				    .result = failure(UserModelStatus::kInvalidShape,
				                      "Stored face model metadata is invalid"),
				};
			}

			UserModelEntry entry{
			    .id      = *id,
			    .time    = *time,
			    .label   = *label,
			    .backend = *backend,
			    .metric  = *metric,
			    .model   = *model_name,
			};
			if (!is_valid_model_label(entry.label)) {
				return EntryParseResult{
				    .result = failure(UserModelStatus::kParseError,
				                      "Model label contains unsafe path characters"),
				};
			}
			if (const auto compatibility = validate_compatibility(entry, expected_backend,
			                                                      expected_metric, expected_model);
			    compatibility.status != UserModelStatus::kOk) {
				return EntryParseResult{.result = compatibility};
			}

			const auto data = model.find("data");
			if (data == model.end() || !data->is_array()) {
				if (strict_shape) {
					return EntryParseResult{
					    .result = failure(UserModelStatus::kInvalidShape,
					                      "Stored face model data is not an array"),
					};
				}
				return EntryParseResult{.entry = std::move(entry)};
			}
			if (data->size() > kMaxEncodingsPerModel) {
				return EntryParseResult{
				    .result = failure(UserModelStatus::kOversized,
				                      "Stored face model contains too many encodings"),
				};
			}

			for (const auto &encoding_json : *data) {
				if (!encoding_json.is_array()) {
					if (strict_shape) {
						return EntryParseResult{
						    .result = failure(UserModelStatus::kInvalidShape,
						                      "Stored face encoding is not an array"),
						};
					}
					continue;
				}
				auto encoding = parse_encoding(encoding_json);
				if (encoding.result.status != UserModelStatus::kOk) {
					return EntryParseResult{.result = std::move(encoding.result)};
				}
				entry.encodings.push_back(std::move(encoding.encoding));
			}

			return EntryParseResult{.entry = std::move(entry)};
		}

		auto parse_entries(const nlohmann::json &models, const std::string &expected_backend,
		                   const std::string &expected_metric, const std::string &expected_model,
		                   bool strict_shape) -> UserModelListResult {
			if (!models.is_array()) {
				return failure(UserModelStatus::kInvalidShape,
				               "Model file is not a valid model list");
			}
			if (models.empty()) {
				return UserModelListResult{.status = UserModelStatus::kNoModel};
			}
			if (models.size() > kMaxStoredModels) {
				return failure(UserModelStatus::kOversized,
				               "Stored face model list exceeds safety limit");
			}

			UserModelListResult result{.status = UserModelStatus::kOk};
			std::set<int>       seen_ids;
			try {
				for (const auto &model : models) {
					auto parsed = parse_model_entry(model, expected_backend, expected_metric,
					                                expected_model, strict_shape);
					if (parsed.result.status != UserModelStatus::kOk) {
						return parsed.result;
					}
					auto &entry = parsed.entry;
					if (strict_shape && !seen_ids.insert(entry.id).second) {
						return failure(UserModelStatus::kInvalidShape,
						               "Stored face model IDs must be unique");
					}
					if (entry.id == std::numeric_limits<int>::max()) {
						if (strict_shape) {
							return failure(UserModelStatus::kInvalidShape,
							               "Stored face model ID is too large");
						}
						result.next_id = std::numeric_limits<int>::max();
					} else {
						result.next_id = std::max(result.next_id, entry.id + 1);
					}
					result.entries.push_back(std::move(parsed.entry));
				}
			} catch (const nlohmann::json::exception &error) {
				return failure(UserModelStatus::kParseError, error.what());
			}
			return result;
		}

		auto load_document_from_path(const std::filesystem::path &path,
		                             const std::string           &expected_backend,
		                             const std::string           &expected_metric,
		                             const std::string &expected_model, bool strict_shape = true)
		    -> ModelDocument {
			std::string regular_error;
			const auto  regular_status = inspect_regular_file_status(path, &regular_error);
			if (regular_status != UserModelStatus::kOk) {
				return ModelDocument{
				    .result = regular_status == UserModelStatus::kParseError
				                  ? failure(regular_status, regular_error)
				                  : UserModelListResult{.status = UserModelStatus::kNoModel},
				};
			}

			std::error_code size_ec;
			const auto      file_size = std::filesystem::file_size(path, size_ec);
			if (size_ec || file_size > kMaxUserModelFileBytes) {
				return ModelDocument{
				    .result =
				        failure(UserModelStatus::kOversized,
				                "User model file is too large or unreadable: " + path.string()),
				};
			}

			std::ifstream input(path);
			if (!input.is_open()) {
				return ModelDocument{
				    .result = failure(UserModelStatus::kParseError,
				                      "Failed to open user model file: " + path.string()),
				};
			}

			nlohmann::json models;
			try {
				input >> models;
			} catch (const nlohmann::json::exception &error) {
				return ModelDocument{
				    .result = failure(UserModelStatus::kParseError, error.what()),
				};
			}
			return ModelDocument{
			    .result = parse_entries(models, expected_backend, expected_metric, expected_model,
			                            strict_shape),
			    .models = std::move(models),
			};
		}

		auto load_entries_from_path(const std::filesystem::path &path,
		                            const std::string           &expected_backend,
		                            const std::string           &expected_metric,
		                            const std::string &expected_model, bool strict_shape = true)
		    -> UserModelListResult {
			return load_document_from_path(path, expected_backend, expected_metric, expected_model,
			                               strict_shape)
			    .result;
		}

		auto entry_to_json(const UserModelEntry &entry) -> nlohmann::json {
			nlohmann::json model = {
			    {"time", entry.time},
			    {"label", entry.label},
			    {"id", entry.id},
			    {"data", entry.encodings},
			};
			if (!entry.backend.empty()) {
				model["backend"] = entry.backend;
			}
			if (!entry.metric.empty()) {
				model["metric"] = entry.metric;
			}
			if (!entry.model.empty()) {
				model["model"] = entry.model;
			}
			return model;
		}

		auto write_models(const std::filesystem::path &path, const nlohmann::json &models) -> bool {
			return write_atomic_file(path, models.dump(), kUserModelFileMode);
		}

		auto load_for_mutation(const std::string &user, ModelPathResult *path_result,
		                       std::optional<ScopedFileLock> *lock) -> ModelDocument {
			*path_result = resolve_model_path(user, true, default_secure_owner_uid());
			if (path_result->status != UserModelStatus::kOk) {
				return ModelDocument{
				    .result = failure(path_result->status, path_result->error_message),
				};
			}

			*lock = acquire_file_lock(path_result->path);
			if (!lock->has_value()) {
				return ModelDocument{
				    .result = failure(UserModelStatus::kLockFailed, "Failed to lock model file"),
				};
			}

			const auto secured_path = resolve_model_path(user, true, default_secure_owner_uid());
			if (secured_path.status != UserModelStatus::kOk) {
				return ModelDocument{
				    .result = failure(secured_path.status, secured_path.error_message),
				};
			}
			return load_document_from_path(path_result->path, {}, {}, {});
		}

		auto lock_existing_model_path(const std::string &user, ModelPathResult *path_result,
		                              std::optional<ScopedFileLock> *lock) -> UserModelStatus {
			*path_result = resolve_model_path(user, false, default_secure_owner_uid());
			if (path_result->status != UserModelStatus::kOk) {
				return path_result->status;
			}
			std::string regular_error;
			const auto  regular_status =
			    inspect_regular_file_status(path_result->path, &regular_error);
			if (regular_status != UserModelStatus::kOk) {
				path_result->error_message = regular_error;
				return regular_status;
			}

			*lock = acquire_file_lock(path_result->path);
			if (!lock->has_value()) {
				path_result->error_message = "Failed to lock model file";
				return UserModelStatus::kLockFailed;
			}

			const auto secured_path = resolve_model_path(user, false, default_secure_owner_uid());
			if (secured_path.status != UserModelStatus::kOk) {
				path_result->error_message = secured_path.error_message;
				return secured_path.status;
			}
			regular_error.clear();
			const auto secured_regular_status =
			    inspect_regular_file_status(path_result->path, &regular_error);
			if (secured_regular_status != UserModelStatus::kOk) {
				path_result->error_message = regular_error;
				return secured_regular_status;
			}
			return UserModelStatus::kOk;
		}

		auto remove_entry_from_document(const std::filesystem::path &path, ModelDocument *document,
		                                UserModelEntry                               removed,
		                                std::vector<UserModelEntry>::difference_type found_index)
		    -> UserModelMutationResult {
			document->models.erase(document->models.begin() + found_index);
			if (document->models.empty()) {
				if (!remove_file_and_sync(path)) {
					return mutation_failure(UserModelStatus::kDeleteFailed,
					                        "Failed to remove model file");
				}
				return UserModelMutationResult{
				    .status       = UserModelStatus::kOk,
				    .entry        = std::move(removed),
				    .removed_last = true,
				};
			}
			if (!write_models(path, document->models)) {
				return mutation_failure(UserModelStatus::kWriteFailed,
				                        "Failed to update model file");
			}
			return UserModelMutationResult{
			    .status = UserModelStatus::kOk,
			    .entry  = std::move(removed),
			};
		}

	}  // namespace

	auto list_user_model_entries(const std::string &user, const std::string &expected_backend,
	                             const std::string &expected_metric,
	                             const std::string &expected_model) -> UserModelListResult {
		const auto path_result = resolve_model_path(user, false, default_secure_owner_uid());
		if (path_result.status != UserModelStatus::kOk) {
			return failure(path_result.status, path_result.error_message);
		}
		return load_entries_from_path(path_result.path, expected_backend, expected_metric,
		                              expected_model);
	}

	auto inspect_user_model_file(const std::string &user) -> UserModelInspectResult {
		const auto path_result = resolve_model_path(user, false, default_secure_owner_uid());
		if (path_result.status != UserModelStatus::kOk) {
			return inspect_failure(path_result.status, path_result.error_message);
		}
		std::string regular_error;
		const auto  regular_status = inspect_regular_file_status(path_result.path, &regular_error);
		if (regular_status != UserModelStatus::kOk) {
			return inspect_failure(regular_status, regular_error);
		}
		const auto snapshot = snapshot_model_file(path_result.path);
		if (!snapshot.has_value()) {
			return inspect_failure(UserModelStatus::kParseError,
			                       "Failed to inspect user model file: " +
			                           path_result.path.string());
		}
		return UserModelInspectResult{
		    .status   = UserModelStatus::kOk,
		    .snapshot = snapshot,
		};
	}

	auto append_user_model_entry(const std::string &user, const NewUserModelEntry &new_entry)
	    -> UserModelMutationResult {
		ModelPathResult               path_result;
		std::optional<ScopedFileLock> lock;
		auto                          document = load_for_mutation(user, &path_result, &lock);
		auto                         &entries  = document.result;
		if (entries.status != UserModelStatus::kOk && entries.status != UserModelStatus::kNoModel) {
			return mutation_failure(entries.status, entries.error_message);
		}
		if (!new_entry.label.empty() && !is_valid_model_label(new_entry.label)) {
			return mutation_failure(UserModelStatus::kInvalidShape,
			                        "New face model entry is invalid");
		}
		if (new_entry.encodings.empty()) {
			return mutation_failure(UserModelStatus::kInvalidShape,
			                        "New face model entry is invalid");
		}
		if (new_entry.encodings.size() > kMaxEncodingsPerModel) {
			return mutation_failure(UserModelStatus::kOversized,
			                        "Stored face model contains too many encodings");
		}
		if (entries.entries.size() >= kMaxStoredModels) {
			return mutation_failure(UserModelStatus::kOversized,
			                        "Stored face model list exceeds safety limit");
		}
		if (entries.next_id >= std::numeric_limits<int>::max()) {
			return mutation_failure(UserModelStatus::kInvalidShape,
			                        "Stored face model ID is too large");
		}
		for (const auto &entry : entries.entries) {
			if (!entry.backend.empty() && entry.backend != new_entry.backend) {
				return mutation_failure(UserModelStatus::kIncompatibleBackend,
				                        "Existing face models use incompatible face-recognition "
				                        "metadata");
			}
			if (!entry.metric.empty() && entry.metric != new_entry.metric) {
				return mutation_failure(UserModelStatus::kIncompatibleMetric,
				                        "Existing face models use incompatible face-recognition "
				                        "metadata");
			}
			if (!entry.model.empty() && entry.model != new_entry.model) {
				return mutation_failure(UserModelStatus::kIncompatibleModel,
				                        "Existing face models use incompatible face-recognition "
				                        "metadata");
			}
		}
		for (const auto &encoding : new_entry.encodings) {
			const auto validation = validate_encoding_values(encoding);
			if (validation.status != UserModelStatus::kOk) {
				return mutation_failure(validation.status, validation.error_message);
			}
		}

		UserModelEntry entry{
		    .id        = entries.next_id,
		    .time      = static_cast<long long>(std::time(nullptr)),
		    .label     = new_entry.label.empty() ? ("Model #" + std::to_string(entries.next_id))
		                                         : new_entry.label,
		    .backend   = new_entry.backend,
		    .metric    = new_entry.metric,
		    .model     = new_entry.model,
		    .encodings = new_entry.encodings,
		};
		document.models.push_back(entry_to_json(entry));
		if (!write_models(path_result.path, document.models)) {
			return mutation_failure(UserModelStatus::kWriteFailed, "Failed to save model file");
		}
		return UserModelMutationResult{.status = UserModelStatus::kOk, .entry = std::move(entry)};
	}

	auto remove_user_model_entry(const std::string &user, int id) -> UserModelMutationResult {
		ModelPathResult               path_result;
		std::optional<ScopedFileLock> lock;
		auto                          document = load_for_mutation(user, &path_result, &lock);
		auto                         &entries  = document.result;
		if (entries.status != UserModelStatus::kOk) {
			return mutation_failure(entries.status, entries.error_message);
		}

		const auto found = std::ranges::find_if(entries.entries, [id](const UserModelEntry &entry) {
			return entry.id == id;
		});
		if (found == entries.entries.end()) {
			return mutation_failure(UserModelStatus::kModelNotFound, "Model ID was not found");
		}

		UserModelEntry removed     = *found;
		const auto     found_index = std::distance(entries.entries.begin(), found);
		return remove_entry_from_document(path_result.path, &document, std::move(removed),
		                                  found_index);
	}

	auto remove_user_model_entry_if_matches(const std::string               &user,
	                                        const UserModelEntryExpectation &expected)
	    -> UserModelMutationResult {
		ModelPathResult               path_result;
		std::optional<ScopedFileLock> lock;
		auto                          document = load_for_mutation(user, &path_result, &lock);
		auto                         &entries  = document.result;
		if (entries.status != UserModelStatus::kOk) {
			return mutation_failure(entries.status, entries.error_message);
		}

		const auto found =
		    std::ranges::find_if(entries.entries, [&expected](const UserModelEntry &entry) {
			    return entry.id == expected.id;
		    });
		if (found == entries.entries.end()) {
			return model_changed_failure();
		}
		if (!entry_matches(*found, expected)) {
			return model_changed_failure();
		}

		UserModelEntry removed     = *found;
		const auto     found_index = std::distance(entries.entries.begin(), found);
		return remove_entry_from_document(path_result.path, &document, std::move(removed),
		                                  found_index);
	}

	auto clear_user_model_entries(const std::string &user) -> UserModelMutationResult {
		ModelPathResult               path_result;
		std::optional<ScopedFileLock> lock;
		const auto                    status = lock_existing_model_path(user, &path_result, &lock);
		if (status != UserModelStatus::kOk) {
			return mutation_failure(status, path_result.error_message);
		}
		if (!remove_file_and_sync(path_result.path)) {
			return mutation_failure(UserModelStatus::kDeleteFailed, "Failed to remove model file");
		}
		return UserModelMutationResult{
		    .status       = UserModelStatus::kOk,
		    .removed_last = true,
		};
	}

	auto clear_user_model_entries_if_unchanged(const std::string           &user,
	                                           const UserModelFileSnapshot &expected_snapshot)
	    -> UserModelMutationResult {
		ModelPathResult               path_result;
		std::optional<ScopedFileLock> lock;
		const auto                    status = lock_existing_model_path(user, &path_result, &lock);
		if (status == UserModelStatus::kNoModel || status == UserModelStatus::kNoModelDirectory) {
			return model_changed_failure();
		}
		if (status != UserModelStatus::kOk) {
			return mutation_failure(status, path_result.error_message);
		}
		const auto current_snapshot = snapshot_model_file(path_result.path);
		if (!current_snapshot.has_value()) {
			return mutation_failure(UserModelStatus::kParseError,
			                        "Failed to inspect user model file: " +
			                            path_result.path.string());
		}
		if (!snapshots_match(*current_snapshot, expected_snapshot)) {
			return model_changed_failure();
		}
		if (!remove_file_and_sync(path_result.path)) {
			return mutation_failure(UserModelStatus::kDeleteFailed, "Failed to remove model file");
		}
		return UserModelMutationResult{
		    .status       = UserModelStatus::kOk,
		    .removed_last = true,
		};
	}

	auto load_user_models(const std::string &user, const std::string &expected_backend)
	    -> UserModelLoadResult {
		return load_user_models(user, expected_backend, default_secure_owner_uid());
	}

	auto load_user_models(const std::string &user, const std::string &expected_backend,
	                      std::optional<uid_t> owner_uid) -> UserModelLoadResult {
		const auto path_result = resolve_model_path(user, false, owner_uid);
		if (path_result.status != UserModelStatus::kOk) {
			return UserModelLoadResult{
			    .status        = path_result.status == UserModelStatus::kNoModelDirectory
			                         ? UserModelStatus::kNoModel
			                         : path_result.status,
			    .error_message = path_result.error_message,
			};
		}

		const auto entries =
		    load_entries_from_path(path_result.path, expected_backend, {}, {}, false);
		UserModelLoadResult result{
		    .status        = entries.status == UserModelStatus::kOversized ||
		                             entries.status == UserModelStatus::kInvalidShape
		                         ? UserModelStatus::kParseError
		                         : entries.status,
		    .error_message = entries.error_message,
		};
		if (entries.status != UserModelStatus::kOk) {
			return result;
		}
		for (const auto &entry : entries.entries) {
			for (const auto &encoding : entry.encodings) {
				result.stored.encodings.push_back(encoding);
				result.stored.models.push_back(EncodingModelInfo{
				    .id    = entry.id,
				    .label = entry.label,
				});
			}
		}
		result.status =
		    result.stored.encodings.empty() ? UserModelStatus::kNoModel : UserModelStatus::kOk;
		return result;
	}

}  // namespace howdy::native
