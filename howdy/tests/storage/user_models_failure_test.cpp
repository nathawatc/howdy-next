#include "storage/user_model_limits.hpp"
#include "storage/user_model_store/test_hooks.hpp"
#include "storage/user_models.hpp"
#include "storage/user_models_test_support.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <latch>
#include <string_view>
#include <thread>
#include <vector>

namespace howdy::test::user_models {
	using namespace howdy::test::user_models;

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

		auto HasStagedModelFile(const std::filesystem::path &models_dir) -> bool {
			return std::ranges::any_of(
			    std::filesystem::directory_iterator(models_dir), [](const auto &entry) -> bool {
				    return entry.path().filename().string().starts_with(".howdy-user-model-");
			    });
		}

		void RemoveStagedModelFiles(const std::filesystem::path &models_dir,
		                            std::error_code             *error) {
			for (const auto &staged_path : StagedUserModelPaths(models_dir)) {
				std::filesystem::remove(staged_path, *error);
				error->clear();
			}
		}

		auto RequireSnapshot(const std::filesystem::path &temp_root, const std::string &user)
		    -> std::optional<howdy::native::UserModelFileSnapshot> {
			const auto inspection = howdy::native::InspectUserModelFile(user, {temp_root});
			if (inspection.status != howdy::native::UserModelStatus::kOk ||
			    !inspection.snapshot.has_value()) {
				return std::nullopt;
			}
			return inspection.snapshot;
		}

		auto ExpectChangedSnapshotComponents(const std::filesystem::path &model_path) -> bool {
			const auto temp_root = model_path.parent_path().parent_path();
			const auto snapshot  = RequireSnapshot(temp_root, "alice");
			if (!snapshot.has_value()) {
				return Expect(false, "capture snapshot for component mismatch tests");
			}

			const auto baseline = *snapshot;
			std::array changed_snapshots{baseline, baseline, baseline, baseline,
			                             baseline, baseline, baseline};
			++changed_snapshots[0].dev;
			++changed_snapshots[1].inode;
			++changed_snapshots[2].size;
			++changed_snapshots[3].mtime_seconds;
			++changed_snapshots[4].mtime_nanosecs;
			++changed_snapshots[5].ctime_seconds;
			++changed_snapshots[6].ctime_nanosecs;

			bool ok = true;
			for (const auto &changed : changed_snapshots) {
				const auto result =
				    howdy::native::ClearUserModelEntriesIfUnchanged("alice", changed, {temp_root});
				ok &= Expect(result.status == howdy::native::UserModelStatus::kModelChanged,
				             "verified clear rejects each changed snapshot component");
				ok &= Expect(std::filesystem::exists(model_path),
				             "changed snapshot leaves model file in place");
			}
			return ok;
		}

		auto ExpectClearMutationFailures(const std::filesystem::path &model_path,
		                                 const std::string           &backend) -> bool {
			const auto temp_root = model_path.parent_path().parent_path();
			bool       ok        = true;
			{
				const auto original_content = ReadFile(model_path);
				const auto snapshot         = RequireSnapshot(temp_root, "alice");
				if (!snapshot.has_value()) {
					return Expect(false, "inspect model file before concurrent-read clear test");
				}
				std::latch                                                    clear_paused(1);
				std::latch                                                    allow_clear(1);
				const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
				    .before_delete_commit = [&](const std::filesystem::path &) -> void {
					    clear_paused.count_down();
					    allow_clear.wait();
				    },
				});
				howdy::native::UserModelMutationResult                        clear_result;
				std::thread                                                   clearer([&] -> void {
					clear_result = howdy::native::ClearUserModelEntriesIfUnchanged(
					    "alice", *snapshot, {temp_root});
				});
				clear_paused.wait();
				const auto during_clear = howdy::native::ListUserModelEntries(
				    "alice", backend, howdy::native::FaceMetric::kCosine, "sface.onnx",
				    {temp_root});
				allow_clear.count_down();
				clearer.join();
				const auto after_clear = howdy::native::ListUserModelEntries(
				    "alice", backend, howdy::native::FaceMetric::kCosine, "sface.onnx",
				    {temp_root});
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
				const auto snapshot         = RequireSnapshot(temp_root, "alice");
				if (!snapshot.has_value()) {
					return Expect(false, "inspect model file before unlink failure test");
				}
				const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
				    .fail_delete_unlink = true,
				});
				const auto result = howdy::native::ClearUserModelEntriesIfUnchanged(
				    "alice", *snapshot, {temp_root});
				ok &= Expect(result.status == howdy::native::UserModelStatus::kDeleteFailed,
				             "clear reports injected unlink failure");
				ok &= Expect(ReadFile(model_path) == original_content,
				             "unlink failure leaves original model at canonical path");
			}
			{
				const auto original_content = ReadFile(model_path);
				const auto snapshot         = RequireSnapshot(temp_root, "alice");
				if (!snapshot.has_value()) {
					return Expect(false, "inspect model file before parent sync failure test");
				}
				const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
				    .fail_parent_sync = true,
				});
				const auto result = howdy::native::ClearUserModelEntriesIfUnchanged(
				    "alice", *snapshot, {temp_root});
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

		auto ExpectUnwritableDirectoryFailures(const std::filesystem::path            &model_path,
		                                       const howdy::native::NewUserModelEntry &second_entry)
		    -> bool {
			bool ok = true;
			WhenSupported(
			    geteuid() != 0,
			    [&] -> void {
				    const auto temp_root            = model_path.parent_path().parent_path();
				    const auto models_dir           = model_path.parent_path();
				    const auto before_failed_writes = ReadFile(model_path);
				    const auto snapshot             = RequireSnapshot(temp_root, "alice");
				    ok &= Expect(snapshot.has_value(),
				                 "inspect model file before unwritable directory checks");
				    ok &= Expect(chmod(models_dir.c_str(), 0555) == 0,
				                 "make models directory unwritable for failed-write checks");
				    const auto append_result =
				        howdy::native::AppendUserModelEntry("alice", second_entry, {temp_root});
				    ok &=
				        Expect(append_result.status == howdy::native::UserModelStatus::kWriteFailed,
				               "append reports atomic write failure");
				    ok &= Expect(ReadFile(model_path) == before_failed_writes,
				                 "failed append write leaves model file unchanged");
				    if (snapshot.has_value()) {
					    const auto clear_result = howdy::native::ClearUserModelEntriesIfUnchanged(
					        "alice", *snapshot, {temp_root});
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

	}  // namespace

	auto TestUserModelMutationFailures() -> bool {
		namespace fs                 = std::filesystem;
		bool              ok         = true;
		const auto        temp_root  = fs::current_path() / "howdy-user-models-test";
		const auto        models_dir = temp_root / "models";
		const auto        model_path = models_dir / "alice.dat";
		const std::string backend    = "opencv_dnn_sface";
		std::error_code   ec;
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
			const auto result = howdy::native::AppendUserModelEntry("alice", entry, {temp_root});
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
			auto incompatible_metric   = second_entry;
			incompatible_metric.metric = howdy::native::FaceMetric::kL2;
			expect_invalid_append(incompatible_metric,
			                      howdy::native::UserModelStatus::kIncompatibleMetric,
			                      "append rejects incompatible metric");
			auto incompatible_model  = second_entry;
			incompatible_model.model = "other.onnx";
			expect_invalid_append(incompatible_model,
			                      howdy::native::UserModelStatus::kIncompatibleModel,
			                      "append rejects incompatible model");
		}
		{
			const auto original_content = ReadFile(model_path);
			ok &= Expect(WriteFile(model_path, MakeMaxModelsDocument()),
			             "write model list at maximum entry count");
			const auto max_result =
			    howdy::native::AppendUserModelEntry("alice", first_entry, {temp_root});
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
			const auto max_id_result =
			    howdy::native::AppendUserModelEntry("alice", first_entry, {temp_root});
			ok &= Expect(max_id_result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "append rejects next model ID at INT_MAX");
			ok &= Expect(WriteFile(model_path, original_content),
			             "restore model after maximum ID test");
		}
		{
			const auto before_failed_remove = ReadFile(model_path);
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .fail_write = true,
			});
			const auto result = howdy::native::RemoveUserModelEntryIfMatches(
			    "alice", first_entry_expectation, {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kWriteFailed,
			             "remove reports staged write failure");
			ok &= Expect(ReadFile(model_path) == before_failed_remove,
			             "failed remove leaves model bytes unchanged");
		}
		{
			const auto original_content = ReadFile(model_path);
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .fail_parent_sync = true,
			});
			const auto result = howdy::native::RemoveUserModelEntryIfMatches(
			    "alice", first_entry_expectation, {temp_root});
			const auto remaining = howdy::native::ListUserModelEntries(
			    "alice", backend, howdy::native::FaceMetric::kCosine, "sface.onnx", {temp_root});
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
				const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
				    .fail_parent_sync = true,
				});
				const auto result = howdy::native::RemoveUserModelEntryIfMatches(
				    "alice", only_expected, {temp_root});
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
			const auto result =
			    howdy::native::AppendUserModelEntry("alice", second_entry, {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kWriteFailed,
			             "unremovable stale model artifact blocks write");
			ok &= Expect(ReadFile(model_path) == original_content && fs::exists(stale_directory),
			             "stale artifact failure preserves canonical model and artifact");
			fs::remove_all(stale_directory, ec);
			ec.clear();
		}
		{
			const auto before_failed_write = ReadFile(model_path);
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .fail_write = true,
			});
			const auto                                                    result =
			    howdy::native::AppendUserModelEntry("alice", second_entry, {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kWriteFailed,
			             "append reports deterministic staged write failure");
			ok &= Expect(ReadFile(model_path) == before_failed_write,
			             "staged write failure leaves previous model bytes unchanged");
		}
		{
			const auto before_failed_fsync = ReadFile(model_path);
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .fail_fsync = true,
			});
			const auto                                                    result =
			    howdy::native::AppendUserModelEntry("alice", second_entry, {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kWriteFailed,
			             "append reports deterministic staged fsync failure");
			ok &= Expect(ReadFile(model_path) == before_failed_fsync,
			             "staged fsync failure leaves previous model bytes unchanged");
		}
		{
			const std::array unsupported_exchange_errors = {ENOSYS, EINVAL, EOPNOTSUPP};
			for (const auto error_number : unsupported_exchange_errors) {
				const auto before_unsupported_exchange = ReadFile(model_path);
				const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
				    .exchange_errno = error_number,
				});
				const auto                                                    result =
				    howdy::native::AppendUserModelEntry("alice", second_entry, {temp_root});
				ok &= Expect(result.status ==
				                 howdy::native::UserModelStatus::kAtomicExchangeUnsupported,
				             "append reports unsupported atomic exchange failure");
				ok &= Expect(result.error_message.contains("filesystem or kernel") &&
				                 result.error_message.contains("atomic model-file exchange"),
				             "append explains unsupported atomic exchange");
				ok &= Expect(ReadFile(model_path) == before_unsupported_exchange,
				             "unsupported atomic exchange leaves model bytes unchanged");
				ok &= Expect(!HasStagedModelFile(models_dir),
				             "unsupported atomic exchange cleans staged model file");
			}
		}
		{
			const auto before_generic_exchange = ReadFile(model_path);
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .exchange_errno = EIO,
			});
			const auto                                                    result =
			    howdy::native::AppendUserModelEntry("alice", second_entry, {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kWriteFailed &&
			                 result.error_message == "Failed to save model file",
			             "generic exchange failure remains a generic write failure");
			ok &= Expect(ReadFile(model_path) == before_generic_exchange,
			             "generic exchange failure leaves model bytes unchanged");
			ok &= Expect(!HasStagedModelFile(models_dir),
			             "generic exchange failure cleans staged model file");
		}
		{
			const auto before_parent_sync_failure = ReadFile(model_path);
			const auto before                     = howdy::native::ListUserModelEntries(
			    "alice", backend, howdy::native::FaceMetric::kCosine, "sface.onnx", {temp_root});
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .fail_parent_sync = true,
			});
			const auto                                                    result =
			    howdy::native::AppendUserModelEntry("alice", second_entry, {temp_root});
			const auto after = howdy::native::ListUserModelEntries(
			    "alice", backend, howdy::native::FaceMetric::kCosine, "sface.onnx", {temp_root});
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
			const auto before           = howdy::native::ListUserModelEntries(
			    "alice", backend, howdy::native::FaceMetric::kCosine, "sface.onnx", {temp_root});
			{
				const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
				    .fail_write_cleanup = true,
				});
				const auto                                                    result =
				    howdy::native::AppendUserModelEntry("alice", second_entry, {temp_root});
				const auto after = howdy::native::ListUserModelEntries(
				    "alice", backend, howdy::native::FaceMetric::kCosine, "sface.onnx",
				    {temp_root});
				ok &= Expect(result.status == howdy::native::UserModelStatus::kOk,
				             "append reports success after committed write with cleanup failure");
				ok &= Expect(before.status == howdy::native::UserModelStatus::kOk &&
				                 after.status == howdy::native::UserModelStatus::kOk &&
				                 after.entries.size() == before.entries.size() + 1,
				             "committed write with cleanup failure remains canonical");
			}
			ok &= Expect(HasStagedModelFile(models_dir),
			             "write cleanup failure leaves injected temporary artifact");
			const auto cleanup_retry =
			    howdy::native::AppendUserModelEntry("alice", second_entry, {temp_root});
			ok &= Expect(cleanup_retry.status == howdy::native::UserModelStatus::kOk,
			             "next append succeeds after stale write cleanup");
			ok &=
			    Expect(!HasStagedModelFile(models_dir), "next append removes stale write artifact");
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
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .before_write_commit = [&hook](const std::filesystem::path &path) -> void {
				    ReplaceWithRegularFileAfterLock(&hook, path);
			    },
			});
			const auto                                                    result =
			    howdy::native::AppendUserModelEntry("alice", second_entry, {temp_root});
			ok &= Expect(hook.calls == 1 && hook.swapped,
			             "swap-during-write hook atomically replaces model before commit");
			ok &= Expect(result.status == howdy::native::UserModelStatus::kWriteFailed,
			             "append rejects swap during staged write commit");
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
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .after_write_identity_check = [&hook](const std::filesystem::path &path) -> void {
				    ReplacePathPreservingOriginal(&hook, path);
			    },
			});
			const auto                                                    result =
			    howdy::native::AppendUserModelEntry("alice", second_entry, {temp_root});
			ok &= Expect(hook.calls == 1 && hook.swapped,
			             "post-check write hook replaces model after final identity check");
			ok &= Expect(result.status == howdy::native::UserModelStatus::kWriteFailed,
			             "append fails when model changes after final write identity check");
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
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .after_write_identity_check = [&hook](const std::filesystem::path &path) -> void {
				    ReplacePathPreservingOriginal(&hook, path);
			    },
			    .rollback_exchange_errno = EINVAL,
			});
			const auto                                                    result =
			    howdy::native::AppendUserModelEntry("alice", second_entry, {temp_root});
			ok &= Expect(hook.calls == 1 && hook.swapped,
			             "rollback-failure hook replaces model after final identity check");
			ok &= Expect(result.status == howdy::native::UserModelStatus::kCommitStateUncertain,
			             "append distinguishes failed post-commit recovery");
			ok &= Expect(result.error_message.contains("inspect state before retrying"),
			             "failed post-commit recovery requires state inspection");
			ok &= Expect(result.entry.id == -1 && !result.removed_last,
			             "uncertain commit result makes no mutation-state claim");
			ok &= Expect(HasStagedModelFile(models_dir),
			             "failed rollback retains staged file for uncertain state");
			ok &= Expect(ReadFile(model_path) != replacement_model,
			             "failed rollback leaves changed canonical namespace visible");

			RemoveStagedModelFiles(models_dir, &ec);
			fs::remove(model_path, ec);
			ec.clear();
			fs::remove(hook.displaced_path, ec);
			ec.clear();
			ok &= Expect(WriteFile(model_path, original_content),
			             "restore model after rollback-failure test");
		}
		ok &= ExpectClearMutationFailures(model_path, backend);
		ok &= ExpectUnwritableDirectoryFailures(model_path, second_entry);

		{
			const howdy::native::UserModelEntryExpectation missing_expected{
			    .id      = 99,
			    .time    = 1,
			    .label   = "missing",
			    .backend = backend,
			    .metric  = howdy::native::FaceMetric::kCosine,
			    .model   = "sface.onnx",
			};
			const auto result = howdy::native::RemoveUserModelEntryIfMatches(
			    "alice", missing_expected, {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kModelChanged,
			             "verified remove reports model changed when expected entry is missing");
		}
		{
			const auto listing = howdy::native::ListUserModelEntries(
			    "alice", backend, howdy::native::FaceMetric::kCosine, "sface.onnx", {temp_root});
			ok &= Expect(listing.status == howdy::native::UserModelStatus::kOk &&
			                 listing.entries.size() == 2,
			             "list entries before verified stale remove");
			const auto expected = ExpectationFromEntry(listing.entries[0]);
			ok &= Expect(
			    WriteFile(
			        model_path,
			        R"([{"id":0,"time":2,"label":"changed","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.1,0.2]],"future_field":"preserved"},{"id":1,"time":1,"label":"second","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.3,0.4]]}])"),
			    "rewrite model entry after remove listing");
			const auto result =
			    howdy::native::RemoveUserModelEntryIfMatches("alice", expected, {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kModelChanged,
			             "verified remove aborts when model entry changes after listing");
			const auto after = howdy::native::ListUserModelEntries(
			    "alice", backend, howdy::native::FaceMetric::kCosine, "sface.onnx", {temp_root});
			ok &= Expect(after.status == howdy::native::UserModelStatus::kOk &&
			                 after.entries.size() == 2 && after.entries[0].label == "changed",
			             "stale verified remove leaves changed model entry");
		}
		ok &= Expect(
		    WriteFile(
		        model_path,
		        R"([{"id":0,"time":1,"label":"first","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.1,0.2]],"future_field":"preserved"},{"id":1,"time":1,"label":"second","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.3,0.4]]}])"),
		    "restore unchanged entries before verified remove");
		{
			const auto listing = howdy::native::ListUserModelEntries(
			    "alice", backend, howdy::native::FaceMetric::kCosine, "sface.onnx", {temp_root});
			ok &= Expect(listing.status == howdy::native::UserModelStatus::kOk &&
			                 listing.entries.size() == 2,
			             "list entries before verified remove");
			const auto expected = ExpectationFromEntry(listing.entries[0]);
			const auto result =
			    howdy::native::RemoveUserModelEntryIfMatches("alice", expected, {temp_root});
			ok &=
			    Expect(result.status == howdy::native::UserModelStatus::kOk && !result.removed_last,
			           "verified remove succeeds when model entry is unchanged");
			ok &= Expect(result.entry.id == 0 && result.entry.label == "first",
			             "verified remove returns actual removed entry");
			const auto remaining =
			    howdy::native::ListUserModelEntries("alice", backend, {}, {}, {temp_root});
			ok &= Expect(remaining.entries.size() == 1 && remaining.entries[0].id == 1,
			             "verified remove preserves other model entries");
		}
		ok &= Expect(
		    WriteFile(
		        model_path,
		        R"([{"id":0,"time":1,"label":"first","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.1,0.2]]},{"id":1,"time":1,"label":"second","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.3,0.4]],"future_field":{"revision":2}}])"),
		    "restore entries before legacy remove");
		{
			const howdy::native::UserModelEntryExpectation expected_0{
			    .id      = 0,
			    .time    = 1,
			    .label   = "first",
			    .backend = backend,
			    .metric  = howdy::native::FaceMetric::kCosine,
			    .model   = "sface.onnx",
			};
			const auto result =
			    howdy::native::RemoveUserModelEntryIfMatches("alice", expected_0, {temp_root});
			ok &=
			    Expect(result.status == howdy::native::UserModelStatus::kOk && !result.removed_last,
			           "remove deletes existing model ID");
			ok &= Expect(result.entry.id == 0 && result.entry.label == "first",
			             "remove returns actual removed entry");
			const auto remaining =
			    howdy::native::ListUserModelEntries("alice", backend, {}, {}, {temp_root});
			ok &= Expect(remaining.entries.size() == 1 && remaining.entries[0].id == 1,
			             "remove preserves other model entries");
			const auto persisted = ReadFile(model_path);
			ok &= Expect(persisted.contains("\"future_field\"") &&
			                 persisted.contains("\"revision\":2"),
			             "remove preserves unknown fields in another entry");
		}
		{
			const howdy::native::UserModelEntryExpectation expected_1{
			    .id      = 1,
			    .time    = 1,
			    .label   = "second",
			    .backend = backend,
			    .metric  = howdy::native::FaceMetric::kCosine,
			    .model   = "sface.onnx",
			};
			const auto result =
			    howdy::native::RemoveUserModelEntryIfMatches("alice", expected_1, {temp_root});
			ok &=
			    Expect(result.status == howdy::native::UserModelStatus::kOk && result.removed_last,
			           "remove deletes last model entry");
			ok &= Expect(!fs::exists(model_path), "remove last entry deletes model file");
		}

		ok &=
		    Expect(howdy::native::AppendUserModelEntry("alice", first_entry, {temp_root}).status ==
		               howdy::native::UserModelStatus::kOk,
		           "append recreates model before clear");
		{
			const auto snapshot = RequireSnapshot(temp_root, "alice");
			ok &= Expect(snapshot.has_value(), "inspect model file before clear");
			if (snapshot.has_value()) {
				const auto result = howdy::native::ClearUserModelEntriesIfUnchanged(
				    "alice", *snapshot, {temp_root});
				ok &= Expect(result.status == howdy::native::UserModelStatus::kOk,
				             "clear removes all model entries");
				ok &= Expect(!fs::exists(model_path), "clear deletes model file");
			}
		}

		const auto created_models_dir = temp_root / "created-store-models";
		fs::remove_all(created_models_dir, ec);
		ec.clear();
		setenv("HOWDY_USER_MODELS_DIR", created_models_dir.c_str(), 1);
		{
			const auto result =
			    howdy::native::AppendUserModelEntry("created-user", first_entry, {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kOk,
			             "append creates missing secure models directory");
			struct stat dir_stat{};
			struct stat file_stat{};
			const auto  created_model_path = created_models_dir / "created-user.dat";
			ok &= Expect(stat(created_models_dir.c_str(), &dir_stat) == 0,
			             "stat append-created models directory");
			ok &= Expect(stat(created_model_path.c_str(), &file_stat) == 0,
			             "stat append-created model file");
			ok &= Expect((dir_stat.st_mode & 0777) == 0750,
			             "append-created models directory uses 0750 mode");
			ok &= Expect((file_stat.st_mode & 0777) == 0600,
			             "append-created model file uses 0600 mode");
		}
		{
			const howdy::native::NewUserModelEntry empty_encoding_entry{
			    .label   = "rejected",
			    .backend = backend,
			    .metric  = howdy::native::FaceMetric::kCosine,
			    .model   = "sface.onnx",
			};
			const auto result = howdy::native::AppendUserModelEntry(
			    "rejected-user", empty_encoding_entry, {temp_root});
			const auto rejected_path = created_models_dir / "rejected-user.dat";
			ok &= Expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "invalid first append is rejected");
			ok &= Expect(!fs::exists(rejected_path),
			             "invalid first append removes empty model artifact");
		}
		fs::remove_all(created_models_dir, ec);
		ec.clear();
		setenv("HOWDY_USER_MODELS_DIR", models_dir.c_str(), 1);

		const auto stale_lock_path = fs::path(model_path.string() + ".lock");
		fs::remove(stale_lock_path, ec);
		ec.clear();
		WhenSupported(
		    symlink("/tmp", stale_lock_path.c_str()) == 0,
		    [&] -> void {
			    const auto result =
			        howdy::native::AppendUserModelEntry("alice", first_entry, {temp_root});
			    ok &= Expect(result.status == howdy::native::UserModelStatus::kOk,
			                 "stale sidecar lock symlink does not affect descriptor locking");
			    ok &= Expect(fs::remove(stale_lock_path, ec), "remove stale lock symlink");
			    ec.clear();
		    },
		    "stale lock symlink creation failed");

		ok &= ExpectChangedSnapshotComponents(model_path);

		fs::remove_all(temp_root, ec);
		unsetenv("HOWDY_USER_MODELS_DIR");
		return ok;
	}

}  // namespace howdy::test::user_models
