#include "storage/user_model_limits.hpp"
#include "storage/user_model_status.hpp"
#include "storage/user_model_store.hpp"
#include "storage/user_model_store/test_hooks.hpp"
#include "storage/user_model_types.hpp"
#include "storage/user_models_test_support.hpp"
#include "support/face_metric.hpp"
#include "test_support.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <latch>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

#include <sys/stat.h>

namespace howdy::test::user_models {

	namespace {

		auto MakeMaxModelsDocument() -> std::string {
			std::string document = "[";
			for (std::size_t index = 0; index < howdy::native::user_model_limits::kMaxStoredModels;
			     ++index) {
				if (index != 0) {
					document += ',';
				}
				document +=
				    R"({"id":)" + std::to_string(index) + R"(,"label":"model","data":[[0.1]]})";
			}
			document += ']';
			return document;
		}

		using howdy::test::CountFilesWithPrefix;

		void RemoveStagedModelFiles(const std::filesystem::path &models_dir,
		                            std::error_code             *error) {
			for (const auto &staged_path : StagedUserModelPaths(models_dir)) {
				std::filesystem::remove(staged_path, *error);
				error->clear();
			}
		}

		auto ExpectChangedSnapshotComponents(const howdy::native::UserModelStore &store,
		                                     const std::filesystem::path &model_path) -> bool {
			const auto snapshot = RequireSnapshot(store, "alice");
			if (!snapshot.has_value()) {
				return Expect(false, "capture snapshot for component mismatch tests");
			}

			const auto baseline = *snapshot;
			std::array changed_snapshots{
			    baseline, baseline, baseline, baseline, baseline, baseline, baseline,
			};
			++changed_snapshots.at(0).dev;
			++changed_snapshots.at(1).inode;
			++changed_snapshots.at(2).size;
			++changed_snapshots.at(3).mtime_seconds;
			++changed_snapshots.at(4).mtime_nanosecs;
			++changed_snapshots.at(5).ctime_seconds;
			++changed_snapshots.at(6).ctime_nanosecs;

			bool ok = true;
			for (const auto &changed : changed_snapshots) {
				const auto result = store.ClearEntriesIfUnchanged("alice", changed);
				ok &= Expect(result.status == howdy::native::UserModelStatus::kModelChanged,
				             "verified clear rejects each changed snapshot component");
				ok &= Expect(std::filesystem::exists(model_path),
				             "changed snapshot leaves model file in place");
			}
			return ok;
		}

		auto ExpectClearMutationFailures(const howdy::native::UserModelStoreConfig &store_config,
		                                 const std::filesystem::path               &model_path,
		                                 const std::string &backend) -> bool {
			const howdy::native::UserModelStore store(store_config);
			bool                                ok = true;
			{
				const auto original_content = ReadFile(model_path);
				const auto snapshot         = RequireSnapshot(store, "alice");
				if (!snapshot.has_value()) {
					return Expect(false, "inspect model file before concurrent-read clear test");
				}
				std::latch clear_paused(1);
				std::latch allow_clear(1);
				const auto hooked_store = WithTestHooks(
				    store_config,
				    {
				        .before_delete_commit = [&](const std::filesystem::path &) -> void {
					        clear_paused.count_down();
					        allow_clear.wait();
				        },
				    });
				howdy::native::UserModelMutationResult clear_result;
				std::thread                            clearer([&] -> void {
					clear_result = hooked_store.ClearEntriesIfUnchanged("alice", *snapshot);
				});
				clear_paused.wait();
				const auto during_clear = store.ListEntries("alice", backend, "sface.onnx");
				allow_clear.count_down();
				clearer.join();
				const auto after_clear = store.ListEntries("alice", backend, "sface.onnx");
				ok &= Expect(during_clear.status == howdy::native::UserModelStatus::kOk &&
				                 !during_clear.entries.empty(),
				             "reader during clear observes original model document");
				ok &= Expect(clear_result.status == howdy::native::UserModelStatus::kOk,
				             "concurrent-read clear succeeds");
				ok &= Expect(after_clear.status == howdy::native::UserModelStatus::kNoModel,
				             "reader after clear observes no model");
				ok &= Expect(WriteFile(model_path, original_content),
				             "restore model after concurrent-read clear test");
			}
			{
				const auto original_content = ReadFile(model_path);
				const auto snapshot         = RequireSnapshot(store, "alice");
				if (!snapshot.has_value()) {
					return Expect(false, "inspect model file before unlink failure test");
				}
				const auto hooked_store = WithTestHooks(store_config, {.fail_delete_unlink = true});
				const auto result       = hooked_store.ClearEntriesIfUnchanged("alice", *snapshot);
				ok &= Expect(result.status == howdy::native::UserModelStatus::kDeleteFailed,
				             "clear reports injected unlink failure");
				ok &= Expect(
				    ReadFile(model_path) == original_content &&
				        (CountFilesWithPrefix(model_path.parent_path(), ".howdy-user-model-") == 0),
				    "unlink failure moves the original model back to its path");
			}
			{
				const auto original_content = ReadFile(model_path);
				const auto snapshot         = RequireSnapshot(store, "alice");
				if (!snapshot.has_value()) {
					return Expect(false, "inspect model file before unsynced move-back test");
				}
				const auto hooked_store =
				    WithTestHooks(store_config, {
				                                    .fail_parent_sync   = true,
				                                    .fail_delete_unlink = true,
				                                });
				const auto result = hooked_store.ClearEntriesIfUnchanged("alice", *snapshot);
				ok &= Expect(result.status == howdy::native::UserModelStatus::kCommitStateUncertain,
				             "clear reports an unsynced move back as uncertain");
				ok &= Expect(ReadFile(model_path) == original_content,
				             "unsynced move back still returns the original model to its path");
			}
			{
				const auto original_content = ReadFile(model_path);
				const auto snapshot         = RequireSnapshot(store, "alice");
				if (!snapshot.has_value()) {
					return Expect(false, "inspect model file before parent sync failure test");
				}
				const auto hooked_store = WithTestHooks(store_config, {.fail_parent_sync = true});
				const auto result       = hooked_store.ClearEntriesIfUnchanged("alice", *snapshot);
				ok &= Expect(result.status == howdy::native::UserModelStatus::kDurabilityUncertain,
				             "clear distinguishes committed parent-sync failure");
				ok &= Expect(result.removed_last,
				             "clear parent-sync failure reports committed removal state");
				ok &= Expect(!std::filesystem::exists(model_path),
				             "clear parent-sync failure leaves model removed from namespace");
				ok &= Expect(WriteFile(model_path, original_content),
				             "restore model after clear parent-sync failure test");
			}
			return ok;
		}

		auto ExpectUnwritableDirectoryFailures(const howdy::native::UserModelStore    &store,
		                                       const std::filesystem::path            &model_path,
		                                       const howdy::native::NewUserModelEntry &second_entry)
		    -> bool {
			bool ok = true;
			WhenSupported(
			    geteuid() != 0,
			    [&] -> void {
				    const auto models_dir           = model_path.parent_path();
				    const auto before_failed_writes = ReadFile(model_path);
				    const auto snapshot             = RequireSnapshot(store, "alice");
				    ok &= Expect(snapshot.has_value(),
					             "inspect model file before unwritable directory checks");
				    ok &= Expect(chmod(models_dir.c_str(), 0555) == 0,
					             "make models directory unwritable for failed-write checks");
				    const auto append_result = store.AppendEntry("alice", second_entry);
				    ok &=
				        Expect(append_result.status == howdy::native::UserModelStatus::kWriteFailed,
						       "append reports atomic write failure");
				    ok &= Expect(ReadFile(model_path) == before_failed_writes,
					             "failed append write leaves model file unchanged");
				    if (snapshot.has_value()) {
					    const auto clear_result = store.ClearEntriesIfUnchanged("alice", *snapshot);
					    ok &= Expect(clear_result.status ==
						                 howdy::native::UserModelStatus::kDeleteFailed,
						             "clear reports failure when parent directory is unwritable");
					    ok &= Expect(ReadFile(model_path) == before_failed_writes,
						             "failed clear leaves model file unchanged");
				    }
				    ok &= Expect(chmod(models_dir.c_str(), 0755) == 0,
					             "restore models directory after failed-write checks");
			    },
			    "atomic write failure checks while running as root");
			return ok;
		}

		// A first enrollment creates nothing at the model path before it commits, so whichever
		// step fails or returns early, the path is left absent.
		auto ExpectFirstEnrollmentFailures(const howdy::native::UserModelStoreConfig &store_config,
		                                   const std::filesystem::path               &models_dir,
		                                   const howdy::native::NewUserModelEntry &entry) -> bool {
			namespace fs = std::filesystem;
			using howdy::native::UserModelStatus;
			using howdy::native::user_model_store_test_hooks::Hooks;

			const howdy::native::UserModelStore store(store_config);
			bool                                ok = true;
			std::error_code                     ec;
			const auto                          model_path = models_dir / "first-user.dat";
			const auto append = [&](const howdy::native::UserModelStore &target_store)
			    -> howdy::native::UserModelMutationResult {
				return target_store.AppendEntry("first-user", entry);
			};
			const auto expect_nothing_left = [&](const std::string &description) -> void {
				struct stat path_stat{};
				ok &= Expect(lstat(model_path.c_str(), &path_stat) != 0 && errno == ENOENT,
				             description + ": nothing at the model path");
				ok &= Expect((CountFilesWithPrefix(models_dir, ".howdy-user-model-") == 0),
				             description + ": no staged file");
			};
			const auto expect_failure = [&](Hooks hooks, UserModelStatus status,
			                                const std::string &description) -> void {
				const auto hooked_store = WithTestHooks(store_config, std::move(hooks));
				ok &= Expect(append(hooked_store).status == status, description + ": status");
				expect_nothing_left(description);
			};

			expect_failure({.fail_write = true}, UserModelStatus::kWriteFailed,
			               "first enrollment staged write failure");
			expect_failure({.fail_fsync = true}, UserModelStatus::kWriteFailed,
			               "first enrollment staged fsync failure");
			expect_failure({.create_errno = EIO}, UserModelStatus::kWriteFailed,
			               "first enrollment link failure");
			for (const auto error_number : {ENOSYS, EINVAL, EOPNOTSUPP}) {
				expect_failure({.create_errno = error_number},
				               UserModelStatus::kAtomicExchangeUnsupported,
				               "first enrollment without RENAME_NOREPLACE support");
			}
			// The early returns between the open and the transaction used to strand an empty
			// placeholder: a directory that fails its second check, and a path that changed.
			const Hooks insecure_directory{
			    .after_lock_before_revalidate = [&](const fs::path &) -> void {
				    ok &= Expect(chmod(models_dir.c_str(), 0777) == 0,
					             "make the models directory world-writable");
			    },
			};
			expect_failure(insecure_directory, UserModelStatus::kInsecurePath,
			               "first enrollment into a directory that fails its second check");
			ok &= Expect(chmod(models_dir.c_str(), 0750) == 0, "restore the models directory");
			{
				const std::string intruder     = "intruder";
				const auto        hooked_store = WithTestHooks(
				    store_config,
				    {
				        .after_lock_before_revalidate = [&](const fs::path &path) -> void {
					        ok &= Expect(WriteFile(path, intruder),
							             "fill the model path after the open");
				        },
				    });
				ok &= Expect(append(hooked_store).status == UserModelStatus::kModelChanged,
				             "first enrollment rejects a path filled after the open");
				ok &= Expect(ReadFile(model_path) == intruder &&
				                 (CountFilesWithPrefix(models_dir, ".howdy-user-model-") == 0),
				             "a path filled after the open is left as it was");
				fs::remove(model_path, ec);
			}
			{
				// RENAME_NOREPLACE refuses a path filled after every check has passed.
				const std::string intruder     = "late intruder";
				const auto        hooked_store = WithTestHooks(
				    store_config,
				    {
				        .after_write_identity_check = [&](const fs::path &path) -> void {
					        ok &= Expect(WriteFile(path, intruder),
							             "fill the model path before commit");
				        },
				    });
				ok &= Expect(append(hooked_store).status == UserModelStatus::kModelChanged,
				             "first enrollment commit refuses to replace a new file");
				ok &= Expect(ReadFile(model_path) == intruder &&
				                 (CountFilesWithPrefix(models_dir, ".howdy-user-model-") == 0),
				             "a refused first enrollment leaves the new file and no staged file");
				fs::remove(model_path, ec);
			}
			{
				const auto hooked_store = WithTestHooks(store_config, {.fail_parent_sync = true});
				const auto result       = append(hooked_store);
				const auto listed = store.ListEntries("first-user", entry.backend, entry.model);
				ok &=
				    Expect(result.status == UserModelStatus::kDurabilityUncertain,
					       "first enrollment reports a parent sync failure as possibly committed");
				ok &= Expect(listed.status == UserModelStatus::kOk && listed.entries.size() == 1,
				             "first enrollment with a parent sync failure is committed");
				fs::remove(model_path, ec);
			}
			{
				bool       absent_before_commit = false;
				const auto hooked_store         = WithTestHooks(
				    store_config, {
				                      .before_write_commit = [&](const fs::path &path) -> void {
					                      struct stat path_stat{};
					                      absent_before_commit =
					                          lstat(path.c_str(), &path_stat) != 0 &&
					                          errno == ENOENT;
				                      },
				                  });
				ok &= Expect(append(hooked_store).status == UserModelStatus::kOk,
				             "first enrollment succeeds");
				ok &= Expect(absent_before_commit,
				             "first enrollment leaves the model path absent until it commits");
				fs::remove(model_path, ec);
			}
			{
				// An empty file, such as an older version's placeholder, is invalid storage.
				ok &= Expect(WriteFile(model_path, "") && chmod(model_path.c_str(), 0600) == 0,
				             "strand an empty model file");
				const auto appended = append(store);
				ok &= Expect(appended.status == UserModelStatus::kParseError &&
				                 appended.error_message.contains("empty") &&
				                 appended.error_message.contains(model_path.string()),
				             "append rejects a stranded empty model file, by name");
				ok &= Expect(store.ListEntries("first-user", entry.backend, entry.model).status ==
				                 UserModelStatus::kParseError,
				             "list rejects a stranded empty model file");
				ok &= Expect(fs::exists(model_path) && fs::file_size(model_path, ec) == 0,
				             "a rejected append leaves the empty model file unchanged");
				fs::remove(model_path, ec);
			}
			return ok;
		}

		// A model removed or replaced after a writer's last identity check is a changed model:
		// before an update's exchange, and before a clear's unlink.
		auto ExpectModelRemovedDuringCommit(const howdy::native::UserModelStoreConfig &store_config,
		                                    const std::filesystem::path               &model_path,
		                                    const howdy::native::NewUserModelEntry &entry) -> bool {
			namespace fs = std::filesystem;
			using howdy::native::UserModelStatus;

			const howdy::native::UserModelStore store(store_config);
			bool                                ok = true;
			std::error_code                     ec;
			const auto                          models_dir       = model_path.parent_path();
			const auto                          original_content = ReadFile(model_path);
			const auto                          remove_model = [&](const fs::path &path) -> void {
				ok &= Expect(fs::remove(path, ec) && !ec, "remove model during commit");
			};
			{
				const auto hooked_store =
				    WithTestHooks(store_config, {.after_write_identity_check = remove_model});
				const auto result = hooked_store.AppendEntry("alice", entry);
				ok &= Expect(result.status == UserModelStatus::kModelChanged,
				             "append reports a model removed before the exchange as changed");
				ok &= Expect(!fs::exists(model_path) &&
				                 (CountFilesWithPrefix(models_dir, ".howdy-user-model-") == 0),
				             "a model removed before the exchange is not recreated");
			}
			ok &= Expect(WriteFile(model_path, original_content),
			             "restore model after removed-before-exchange test");
			const auto snapshot = RequireSnapshot(store, "alice");
			ok &= Expect(snapshot.has_value(), "inspect model file before removed-model clear");
			if (snapshot.has_value()) {
				const auto hooked_store =
				    WithTestHooks(store_config, {.before_delete_commit = remove_model});
				ok &= Expect(hooked_store.ClearEntriesIfUnchanged("alice", *snapshot).status ==
				                 UserModelStatus::kModelChanged,
				             "clear reports a model removed before its unlink as changed");
			}
			ok &= Expect(WriteFile(model_path, original_content),
			             "restore model after removed-before-unlink test");

			// A file put at the path after the identity check is moved back, not unlinked.
			const auto replacement     = std::string("replacement\n");
			const auto replaced_before = RequireSnapshot(store, "alice");
			ok &= Expect(replaced_before.has_value(), "inspect model file before replaced clear");
			if (replaced_before.has_value()) {
				const auto hooked_store = WithTestHooks(
				    store_config,
				    {
				        .before_delete_commit = [&](const fs::path &path) -> void {
					        ok &= Expect(fs::remove(path, ec) && WriteFile(path, replacement),
							             "replace model during clear");
				        },
				    });
				ok &=
				    Expect(hooked_store.ClearEntriesIfUnchanged("alice", *replaced_before).status ==
					           UserModelStatus::kModelChanged,
					       "clear reports a model replaced before its unlink as changed");
			}
			ok &= Expect(ReadFile(model_path) == replacement &&
			                 (CountFilesWithPrefix(models_dir, ".howdy-user-model-") == 0),
			             "clear leaves a replacement made before its unlink in place");
			fs::remove(model_path, ec);
			ok &= Expect(WriteFile(model_path, original_content),
			             "restore model after replaced-before-unlink test");

			// A removal's aside name must fit even when the model's own name is near NAME_MAX.
			const auto long_user     = std::string(240, 'l');
			const auto long_appended = store.AppendEntry(long_user, entry);
			ok &= Expect(long_appended.status == UserModelStatus::kOk,
			             "append a first model for a long user name");
			const auto long_snapshot = RequireSnapshot(store, long_user);
			ok &= Expect(long_snapshot.has_value(), "inspect a long user name's model file");
			if (long_snapshot.has_value()) {
				ok &= Expect(store.ClearEntriesIfUnchanged(long_user, *long_snapshot).status ==
				                 UserModelStatus::kOk,
				             "clear removes a long user name's model");
			}
			ok &= Expect(!fs::exists(models_dir / (long_user + ".dat")) &&
			                 (CountFilesWithPrefix(models_dir, ".howdy-user-model-") == 0),
			             "clearing a long user name's model leaves nothing behind");
			return ok;
		}

		// The canonical file must still exist and hold the mutated entry, not the replacement.
		auto CanonicalShowsChangedEntry(const std::filesystem::path &model_path,
		                                const std::string           &replacement) -> bool {
			if (!std::filesystem::exists(model_path)) {
				return false;
			}
			const auto content = ReadFile(model_path);
			return content != replacement && content.contains("\"second\"");
		}

	}  // namespace

	auto TestUserModelMutationFailures() -> bool {
		namespace fs            = std::filesystem;
		bool       ok           = true;
		const auto temp_root    = fs::current_path() / "howdy-user-models-test";
		const auto models_dir   = temp_root / "models";
		const auto model_path   = models_dir / "alice.dat";
		const auto store_config = MakeTestStoreConfig(models_dir, temp_root);
		const howdy::native::UserModelStore    store(store_config);
		const std::string                      backend = "opencv_dnn_sface";
		std::error_code                        ec;
		const howdy::native::NewUserModelEntry first_entry{
		    .label     = "first",
		    .backend   = backend,
		    .metric    = howdy::native::FaceMetric::kCosine,
		    .model     = "sface.onnx",
		    .encodings = {{0.1F, 0.2F}},
		};
		const howdy::native::NewUserModelEntry second_entry{
		    .label     = "second",
		    .backend   = backend,
		    .metric    = howdy::native::FaceMetric::kCosine,
		    .model     = "sface.onnx",
		    .encodings = {{0.3F, 0.4F}},
		};
		const howdy::native::UserModelEntryExpectation first_entry_expectation{
		    .id      = 0,
		    .time    = 1,
		    .label   = "first",
		    .backend = backend,
		    .metric  = howdy::native::FaceMetric::kCosine,
		    .model   = "sface.onnx",
		};
		const auto expect_invalid_append = [&](const howdy::native::NewUserModelEntry &entry,
		                                       howdy::native::UserModelStatus          status,
		                                       std::string_view description) -> void {
			const auto before = ReadFile(model_path);
			const auto result = store.AppendEntry("alice", entry);
			ok &= Expect(result.status == status, std::string(description) + ": status");
			ok &= Expect(ReadFile(model_path) == before,
			             std::string(description) + ": model bytes unchanged");
		};
		{
			const howdy::native::NewUserModelEntry invalid_label{
			    .label     = "bad/name",
			    .backend   = backend,
			    .metric    = howdy::native::FaceMetric::kCosine,
			    .model     = "sface.onnx",
			    .encodings = {{0.1F}},
			};
			expect_invalid_append(invalid_label, howdy::native::UserModelStatus::kInvalidShape,
			                      "append rejects unsafe label");

			const howdy::native::NewUserModelEntry empty_encoding{
			    .label   = "empty",
			    .backend = backend,
			    .metric  = howdy::native::FaceMetric::kCosine,
			    .model   = "sface.onnx",
			};
			expect_invalid_append(empty_encoding, howdy::native::UserModelStatus::kInvalidShape,
			                      "append rejects empty encodings");

			auto too_many_encodings = first_entry;
			too_many_encodings.encodings.assign(
			    howdy::native::user_model_limits::kMaxEncodingsPerModel + 1, {0.1F});
			expect_invalid_append(too_many_encodings, howdy::native::UserModelStatus::kOversized,
			                      "append rejects too many encodings");

			auto incompatible_backend    = second_entry;
			incompatible_backend.backend = "other_backend";
			expect_invalid_append(incompatible_backend,
			                      howdy::native::UserModelStatus::kIncompatibleBackend,
			                      "append rejects incompatible backend");
			auto incompatible_model  = second_entry;
			incompatible_model.model = "other.onnx";
			expect_invalid_append(incompatible_model,
			                      howdy::native::UserModelStatus::kIncompatibleModel,
			                      "append rejects incompatible model");
		}
		{
			// Stored embeddings are raw SFace output, so the metric only affects matching.
			const auto original_content = ReadFile(model_path);
			auto       other_metric     = second_entry;
			other_metric.metric         = howdy::native::FaceMetric::kL2;
			const auto result           = store.AppendEntry("alice", other_metric);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kOk,
			             "append accepts a different metric");
			ok &= Expect(WriteFile(model_path, original_content),
			             "restore model after different-metric append");
		}
		{
			const auto original_content = ReadFile(model_path);
			ok &= Expect(WriteFile(model_path, MakeMaxModelsDocument()),
			             "write model list at maximum entry count");
			const auto max_result = store.AppendEntry("alice", first_entry);
			ok &= Expect(max_result.status == howdy::native::UserModelStatus::kOversized,
			             "append rejects model list at maximum entry count");
			ok &= Expect(WriteFile(model_path, original_content),
			             "restore model after maximum entry count test");
		}
		{
			const auto original_content = ReadFile(model_path);
			ok &= Expect(
			    WriteFile(model_path, R"([{"id":2147483646,"label":"near-max","data":[[0.1]]}])"),
			    "write model with next ID at INT_MAX");
			const auto max_id_result = store.AppendEntry("alice", first_entry);
			ok &= Expect(max_id_result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "append rejects next model ID at INT_MAX");
			ok &= Expect(WriteFile(model_path, original_content),
			             "restore model after maximum ID test");
		}
		{
			const auto before_failed_remove = ReadFile(model_path);
			const auto hooked_store         = WithTestHooks(store_config, {.fail_write = true});
			const auto result = hooked_store.RemoveEntryIfMatches("alice", first_entry_expectation);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kWriteFailed,
			             "remove reports staged write failure");
			ok &= Expect(ReadFile(model_path) == before_failed_remove,
			             "failed remove leaves model bytes unchanged");
		}
		{
			const auto original_content = ReadFile(model_path);
			const auto hooked_store     = WithTestHooks(store_config, {.fail_parent_sync = true});
			const auto result = hooked_store.RemoveEntryIfMatches("alice", first_entry_expectation);
			const auto remaining = store.ListEntries("alice", backend, "sface.onnx");
			ok &= Expect(result.status == howdy::native::UserModelStatus::kDurabilityUncertain,
			             "remove distinguishes committed parent-sync failure");
			ok &= Expect(!result.removed_last &&
			                 remaining.status == howdy::native::UserModelStatus::kOk &&
			                 remaining.entries.size() == 1,
			             "non-last remove parent-sync failure leaves updated model visible");
			ok &= Expect(WriteFile(model_path, original_content),
			             "restore model after non-last remove parent-sync failure");
		}
		{
			const auto original_content = ReadFile(model_path);
			ok &= Expect(WriteFile(model_path, R"([{"id":0,"label":"only","data":[[0.1]]}])"),
			             "write one-entry model before last remove failure");
			{
				const howdy::native::UserModelEntryExpectation only_expected{
				    .id      = 0,
				    .time    = 0,
				    .label   = "only",
				    .backend = "",
				    .metric  = std::nullopt,
				    .model   = "",
				};
				const auto hooked_store = WithTestHooks(store_config, {.fail_parent_sync = true});
				const auto result       = hooked_store.RemoveEntryIfMatches("alice", only_expected);
				ok &= Expect(result.status == howdy::native::UserModelStatus::kDurabilityUncertain,
				             "last remove distinguishes committed parent-sync failure");
				ok &= Expect(result.removed_last && !fs::exists(model_path),
				             "last remove reports committed deletion state");
			}
			ok &= Expect(WriteFile(model_path, original_content),
			             "restore model after last remove parent-sync failure");
		}
		{
			const auto original_content = ReadFile(model_path);
			const auto stale_directory  = models_dir / ".howdy-user-model-stale";
			fs::remove_all(stale_directory, ec);
			ec.clear();
			ok &= Expect(fs::create_directory(stale_directory, ec) && !ec,
			             "create stale model artifact directory");
			const auto result = store.AppendEntry("alice", second_entry);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kWriteFailed,
			             "unremovable stale model artifact blocks write");
			ok &= Expect(ReadFile(model_path) == original_content && fs::exists(stale_directory),
			             "stale artifact failure preserves canonical model and artifact");
			fs::remove_all(stale_directory, ec);
			ec.clear();
		}
		{
			const auto before_failed_write = ReadFile(model_path);
			const auto hooked_store        = WithTestHooks(store_config, {.fail_write = true});
			const auto result              = hooked_store.AppendEntry("alice", second_entry);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kWriteFailed,
			             "append reports deterministic staged write failure");
			ok &= Expect(ReadFile(model_path) == before_failed_write,
			             "staged write failure leaves previous model bytes unchanged");
		}
		{
			const auto before_failed_fsync = ReadFile(model_path);
			const auto hooked_store        = WithTestHooks(store_config, {.fail_fsync = true});
			const auto result              = hooked_store.AppendEntry("alice", second_entry);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kWriteFailed,
			             "append reports deterministic staged fsync failure");
			ok &= Expect(ReadFile(model_path) == before_failed_fsync,
			             "staged fsync failure leaves previous model bytes unchanged");
		}
		{
			const std::array unsupported_exchange_errors = {ENOSYS, EINVAL, EOPNOTSUPP};
			for (const auto error_number : unsupported_exchange_errors) {
				const auto before_unsupported_exchange = ReadFile(model_path);
				const auto hooked_store =
				    WithTestHooks(store_config, {.exchange_errno = error_number});
				const auto result = hooked_store.AppendEntry("alice", second_entry);
				ok &= Expect(result.status ==
				                 howdy::native::UserModelStatus::kAtomicExchangeUnsupported,
				             "append reports unsupported atomic exchange failure");
				ok &= Expect(result.error_message.contains("filesystem or kernel") &&
				                 result.error_message.contains("atomic rename (renameat2)"),
				             "append explains unsupported atomic exchange");
				ok &= Expect(ReadFile(model_path) == before_unsupported_exchange,
				             "unsupported atomic exchange leaves model bytes unchanged");
				ok &= Expect((CountFilesWithPrefix(models_dir, ".howdy-user-model-") == 0),
				             "unsupported atomic exchange cleans staged model file");
			}
		}
		{
			const auto before_generic_exchange = ReadFile(model_path);
			const auto hooked_store = WithTestHooks(store_config, {.exchange_errno = EIO});
			const auto result       = hooked_store.AppendEntry("alice", second_entry);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kWriteFailed &&
			                 result.error_message == "Failed to save model file",
			             "generic exchange failure remains a generic write failure");
			ok &= Expect(ReadFile(model_path) == before_generic_exchange,
			             "generic exchange failure leaves model bytes unchanged");
			ok &= Expect((CountFilesWithPrefix(models_dir, ".howdy-user-model-") == 0),
			             "generic exchange failure cleans staged model file");
		}
		{
			const auto before_parent_sync_failure = ReadFile(model_path);
			const auto before       = store.ListEntries("alice", backend, "sface.onnx");
			const auto hooked_store = WithTestHooks(store_config, {.fail_parent_sync = true});
			const auto result       = hooked_store.AppendEntry("alice", second_entry);
			const auto after        = store.ListEntries("alice", backend, "sface.onnx");
			ok &= Expect(result.status == howdy::native::UserModelStatus::kDurabilityUncertain,
			             "append distinguishes committed parent-sync failure");
			ok &= Expect(result.error_message.contains("verify state before retrying"),
			             "append parent-sync failure warns before retry");
			ok &= Expect(before.status == howdy::native::UserModelStatus::kOk &&
			                 after.status == howdy::native::UserModelStatus::kOk &&
			                 after.entries.size() == before.entries.size() + 1,
			             "append parent-sync failure leaves committed model visible");
			ok &= Expect(WriteFile(model_path, before_parent_sync_failure),
			             "restore model after append parent-sync failure test");
		}
		{
			const auto original_content = ReadFile(model_path);
			const auto before           = store.ListEntries("alice", backend, "sface.onnx");
			{
				const auto hooked_store = WithTestHooks(store_config, {.fail_write_cleanup = true});
				const auto result       = hooked_store.AppendEntry("alice", second_entry);
				const auto after        = store.ListEntries("alice", backend, "sface.onnx");
				ok &= Expect(result.status == howdy::native::UserModelStatus::kOk,
				             "append reports success after committed write with cleanup failure");
				ok &= Expect(before.status == howdy::native::UserModelStatus::kOk &&
				                 after.status == howdy::native::UserModelStatus::kOk &&
				                 after.entries.size() == before.entries.size() + 1,
				             "committed write with cleanup failure remains canonical");
			}
			ok &= Expect((CountFilesWithPrefix(models_dir, ".howdy-user-model-") != 0),
			             "write cleanup failure leaves injected temporary artifact");
			const auto cleanup_retry = store.AppendEntry("alice", second_entry);
			ok &= Expect(cleanup_retry.status == howdy::native::UserModelStatus::kOk,
			             "next append succeeds after stale write cleanup");
			ok &= Expect((CountFilesWithPrefix(models_dir, ".howdy-user-model-") == 0),
			             "next append removes stale write artifact");
			fs::remove(model_path, ec);
			ec.clear();
			ok &= Expect(WriteFile(model_path, original_content),
			             "restore model after write cleanup failure test");
		}
		{
			const auto before_swap_during_write = ReadFile(model_path);
			const auto replacement_path         = temp_root / "swap-during-write-model.dat";
			const auto replacement_model        = std::string(
			    R"([{"id":0,"time":1,"label":"swap-during-write","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.4,0.5]]}])");
			ok &= Expect(WriteFile(replacement_path, replacement_model),
			             "write replacement before swap-during-write test");
			PostLockRegularFileSwap hook{.replacement_path = replacement_path};
			const auto              hooked_store = WithTestHooks(
			    store_config,
			    {
			        .before_write_commit = [&hook](const std::filesystem::path &path) -> void {
				        ReplaceWithRegularFileAfterLock(&hook, path);
			        },
			    });
			const auto result = hooked_store.AppendEntry("alice", second_entry);
			ok &= Expect(hook.calls == 1 && hook.swapped,
			             "swap-during-write hook atomically replaces model before commit");
			ok &= Expect(result.status == howdy::native::UserModelStatus::kModelChanged,
			             "append reports a swap during the staged write as a changed model");
			ok &= Expect(ReadFile(model_path) == replacement_model,
			             "swap-during-write replacement remains unmodified");
			fs::remove(model_path, ec);
			ec.clear();
			ok &= Expect(WriteFile(model_path, before_swap_during_write),
			             "restore model after swap-during-write test");
		}
		{
			const auto before_swap_after_check = ReadFile(model_path);
			const auto replacement_model       = std::string(
			    R"([{"id":0,"time":1,"label":"swap-after-write-check","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.6,0.7]]}])");
			PreservingRegularFileSwap hook{
			    .replacement_path = temp_root / "swap-after-write-check-replacement.dat",
			    .displaced_path   = temp_root / "swap-after-write-check-original.dat",
			};
			ok &= Expect(WriteFile(hook.replacement_path, replacement_model),
			             "write replacement before post-check write swap test");
			const auto hooked_store = WithTestHooks(
			    store_config, {
			                      .after_write_identity_check =
			                          [&hook](const std::filesystem::path &path) -> void {
				                      ReplacePathPreservingOriginal(&hook, path);
			                      },
			                  });
			const auto result = hooked_store.AppendEntry("alice", second_entry);
			ok &= Expect(hook.calls == 1 && hook.swapped && !hook.rollback_failed,
			             "post-check write hook replaces model after final identity check");
			ok &=
			    Expect(result.status == howdy::native::UserModelStatus::kModelChanged,
				       "append reports a model replaced after the final identity check as changed");
			ok &= Expect(ReadFile(model_path) == replacement_model,
			             "failed post-check write leaves replacement model unchanged");
			ok &= Expect(ReadFile(hook.displaced_path) == before_swap_after_check,
			             "failed post-check write leaves displaced locked model unchanged");
			fs::remove(model_path, ec);
			ec.clear();
			fs::remove(hook.displaced_path, ec);
			ec.clear();
			ok &= Expect(WriteFile(model_path, before_swap_after_check),
			             "restore model after post-check write swap test");
		}
		{
			const auto original_content  = ReadFile(model_path);
			const auto replacement_model = std::string(
			    R"([{"id":0,"time":1,"label":"rollback-failure-replacement","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.8,0.9]]}])");
			PreservingRegularFileSwap hook{
			    .replacement_path = temp_root / "rollback-failure-replacement.dat",
			    .displaced_path   = temp_root / "rollback-failure-original.dat",
			};
			ok &= Expect(WriteFile(hook.replacement_path, replacement_model),
			             "write replacement before rollback-failure test");
			const auto hooked_store = WithTestHooks(
			    store_config, {
			                      .after_write_identity_check =
			                          [&hook](const std::filesystem::path &path) -> void {
				                      ReplacePathPreservingOriginal(&hook, path);
			                      },
			                      .rollback_exchange_errno = EINVAL,
			                  });
			const auto result = hooked_store.AppendEntry("alice", second_entry);
			ok &= Expect(hook.calls == 1 && hook.swapped && !hook.rollback_failed,
			             "rollback-failure hook replaces model after final identity check");
			ok &= Expect(result.status == howdy::native::UserModelStatus::kCommitStateUncertain,
			             "append distinguishes failed post-commit recovery");
			ok &= Expect(result.error_message.contains("inspect state before retrying"),
			             "failed post-commit recovery requires state inspection");
			ok &= Expect(result.entry.id == -1 && !result.removed_last,
			             "uncertain commit result makes no mutation-state claim");
			ok &= Expect((CountFilesWithPrefix(models_dir, ".howdy-user-model-") != 0),
			             "failed rollback retains staged file for uncertain state");
			ok &= Expect(CanonicalShowsChangedEntry(model_path, replacement_model),
			             "failed rollback leaves changed canonical namespace visible");

			RemoveStagedModelFiles(models_dir, &ec);
			fs::remove(model_path, ec);
			ec.clear();
			fs::remove(hook.displaced_path, ec);
			ec.clear();
			ok &= Expect(WriteFile(model_path, original_content),
			             "restore model after rollback-failure test");
		}
		ok &= ExpectModelRemovedDuringCommit(store_config, model_path, second_entry);
		ok &= ExpectClearMutationFailures(store_config, model_path, backend);
		ok &= ExpectUnwritableDirectoryFailures(store, model_path, second_entry);

		const auto first_enroll_dir = temp_root / "first-enrollment-failures";
		fs::remove_all(first_enroll_dir, ec);
		ec.clear();
		fs::create_directories(first_enroll_dir, ec);
		ok &= Expect(!ec, "create first-enrollment failure models directory");
		ec.clear();
		const auto first_enroll_config = MakeTestStoreConfig(first_enroll_dir, temp_root);
		ok &= ExpectFirstEnrollmentFailures(first_enroll_config, first_enroll_dir, first_entry);
		fs::remove_all(first_enroll_dir, ec);
		ec.clear();

		ok &= ExpectChangedSnapshotComponents(store, model_path);

		fs::remove_all(temp_root, ec);
		return ok;
	}

}  // namespace howdy::test::user_models
