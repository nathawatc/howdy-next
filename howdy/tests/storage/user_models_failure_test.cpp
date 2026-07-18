#include "storage/user_model_store_test_hooks.hpp"
#include "storage/user_models_test_support.hpp"

#include <latch>
#include <thread>

namespace howdy::test::user_models {
	using namespace howdy::test::user_models;

	auto test_user_model_mutation_failures() -> bool {
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
		    .metric    = "cosine",
		    .model     = "sface.onnx",
		    .encodings = {{0.1F, 0.2F}},
		};
		const howdy::native::NewUserModelEntry second_entry{
		    .label     = "second",
		    .backend   = backend,
		    .metric    = "cosine",
		    .model     = "sface.onnx",
		    .encodings = {{0.3F, 0.4F}},
		};
		{
			const auto before_failed_write = read_file(model_path);
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .fail_write = true,
			});
			const auto result = howdy::native::append_user_model_entry("alice", second_entry);
			ok &= expect(result.status == howdy::native::UserModelStatus::kWriteFailed,
			             "append reports deterministic staged write failure");
			ok &= expect(read_file(model_path) == before_failed_write,
			             "staged write failure leaves previous model bytes unchanged");
		}
		{
			const auto before_failed_fsync = read_file(model_path);
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .fail_fsync = true,
			});
			const auto result = howdy::native::append_user_model_entry("alice", second_entry);
			ok &= expect(result.status == howdy::native::UserModelStatus::kWriteFailed,
			             "append reports deterministic staged fsync failure");
			ok &= expect(read_file(model_path) == before_failed_fsync,
			             "staged fsync failure leaves previous model bytes unchanged");
		}
		{
			const auto before_parent_sync_failure = read_file(model_path);
			const auto before =
			    howdy::native::list_user_model_entries("alice", backend, "cosine", "sface.onnx");
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .fail_parent_sync = true,
			});
			const auto result = howdy::native::append_user_model_entry("alice", second_entry);
			const auto after =
			    howdy::native::list_user_model_entries("alice", backend, "cosine", "sface.onnx");
			ok &= expect(result.status == howdy::native::UserModelStatus::kDurabilityUncertain,
			             "append distinguishes committed parent-sync failure");
			ok &= expect(result.error_message.contains("verify state before retrying"),
			             "append parent-sync failure warns before retry");
			ok &= expect(before.status == howdy::native::UserModelStatus::kOk &&
			                 after.status == howdy::native::UserModelStatus::kOk &&
			                 after.entries.size() == before.entries.size() + 1,
			             "append parent-sync failure leaves committed model visible");
			ok &= expect(write_file(model_path, before_parent_sync_failure),
			             "restore model after append parent-sync failure test");
		}
		{
			const auto original_content = read_file(model_path);
			const auto before =
			    howdy::native::list_user_model_entries("alice", backend, "cosine", "sface.onnx");
			{
				const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
				    .fail_write_cleanup = true,
				});
				const auto result = howdy::native::append_user_model_entry("alice", second_entry);
				const auto after  = howdy::native::list_user_model_entries("alice", backend,
				                                                           "cosine", "sface.onnx");
				ok &= expect(result.status == howdy::native::UserModelStatus::kOk,
				             "append reports success after committed write with cleanup failure");
				ok &= expect(before.status == howdy::native::UserModelStatus::kOk &&
				                 after.status == howdy::native::UserModelStatus::kOk &&
				                 after.entries.size() == before.entries.size() + 1,
				             "committed write with cleanup failure remains canonical");
			}
			bool found_write_temp = false;
			for (const auto &entry : fs::directory_iterator(models_dir)) {
				found_write_temp |=
				    entry.path().filename().string().starts_with(".howdy-user-model-");
			}
			ok &= expect(found_write_temp,
			             "write cleanup failure leaves injected temporary artifact");
			const auto cleanup_retry =
			    howdy::native::append_user_model_entry("alice", second_entry);
			ok &= expect(cleanup_retry.status == howdy::native::UserModelStatus::kOk,
			             "next append succeeds after stale write cleanup");
			found_write_temp = false;
			for (const auto &entry : fs::directory_iterator(models_dir)) {
				found_write_temp |=
				    entry.path().filename().string().starts_with(".howdy-user-model-");
			}
			ok &= expect(!found_write_temp, "next append removes stale write artifact");
			fs::remove(model_path, ec);
			ec.clear();
			ok &= expect(write_file(model_path, original_content),
			             "restore model after write cleanup failure test");
		}
		{
			const auto before_swap_during_write = read_file(model_path);
			const auto replacement_path         = temp_root / "swap-during-write-model.dat";
			const auto replacement_model        = std::string(
			    R"([{"id":0,"time":1,"label":"swap-during-write","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.4,0.5]]}])");
			ok &= expect(write_file(replacement_path, replacement_model),
			             "write replacement before swap-during-write test");
			PostLockRegularFileSwap hook{.replacement_path = replacement_path};
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .before_write_commit = [&hook](const std::filesystem::path &path) -> void {
				    replace_with_regular_file_after_lock(&hook, path);
			    },
			});
			const auto result = howdy::native::append_user_model_entry("alice", second_entry);
			ok &= expect(hook.calls == 1 && hook.swapped,
			             "swap-during-write hook atomically replaces model before commit");
			ok &= expect(result.status == howdy::native::UserModelStatus::kWriteFailed,
			             "append rejects swap during staged write commit");
			ok &= expect(read_file(model_path) == replacement_model,
			             "swap-during-write replacement remains unmodified");
			fs::remove(model_path, ec);
			ec.clear();
			ok &= expect(write_file(model_path, before_swap_during_write),
			             "restore model after swap-during-write test");
		}
		{
			const auto before_swap_after_check = read_file(model_path);
			const auto replacement_model       = std::string(
			    R"([{"id":0,"time":1,"label":"swap-after-write-check","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.6,0.7]]}])");
			PreservingRegularFileSwap hook{
			    .replacement_path = temp_root / "swap-after-write-check-replacement.dat",
			    .displaced_path   = temp_root / "swap-after-write-check-original.dat",
			};
			ok &= expect(write_file(hook.replacement_path, replacement_model),
			             "write replacement before post-check write swap test");
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .after_write_identity_check = [&hook](const std::filesystem::path &path) -> void {
				    replace_path_preserving_original(&hook, path);
			    },
			});
			const auto result = howdy::native::append_user_model_entry("alice", second_entry);
			ok &= expect(hook.calls == 1 && hook.swapped,
			             "post-check write hook replaces model after final identity check");
			ok &= expect(result.status == howdy::native::UserModelStatus::kWriteFailed,
			             "append fails when model changes after final write identity check");
			ok &= expect(read_file(model_path) == replacement_model,
			             "failed post-check write leaves replacement model unchanged");
			ok &= expect(read_file(hook.displaced_path) == before_swap_after_check,
			             "failed post-check write leaves displaced locked model unchanged");
			fs::remove(model_path, ec);
			ec.clear();
			fs::remove(hook.displaced_path, ec);
			ec.clear();
			ok &= expect(write_file(model_path, before_swap_after_check),
			             "restore model after post-check write swap test");
		}
		{
			const auto original_content  = read_file(model_path);
			const auto replacement_model = std::string(
			    R"([{"id":0,"time":1,"label":"rollback-failure-replacement","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.8,0.9]]}])");
			PreservingRegularFileSwap hook{
			    .replacement_path = temp_root / "rollback-failure-replacement.dat",
			    .displaced_path   = temp_root / "rollback-failure-original.dat",
			};
			ok &= expect(write_file(hook.replacement_path, replacement_model),
			             "write replacement before rollback-failure test");
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .after_write_identity_check = [&hook](const std::filesystem::path &path) -> void {
				    replace_path_preserving_original(&hook, path);
			    },
			    .fail_write_rollback = true,
			});
			const auto result = howdy::native::append_user_model_entry("alice", second_entry);
			ok &= expect(hook.calls == 1 && hook.swapped,
			             "rollback-failure hook replaces model after final identity check");
			ok &= expect(result.status == howdy::native::UserModelStatus::kCommitStateUncertain,
			             "append distinguishes failed post-commit recovery");
			ok &= expect(result.error_message.contains("inspect state before retrying"),
			             "failed post-commit recovery requires state inspection");
			ok &= expect(result.entry.id == -1 && !result.removed_last,
			             "uncertain commit result makes no mutation-state claim");
			ok &= expect(read_file(model_path) != replacement_model,
			             "failed rollback leaves changed canonical namespace visible");

			const auto staged_paths = staged_user_model_paths(models_dir);
			for (const auto &staged_path : staged_paths) {
				fs::remove(staged_path, ec);
				ec.clear();
			}
			fs::remove(model_path, ec);
			ec.clear();
			fs::remove(hook.displaced_path, ec);
			ec.clear();
			ok &= expect(write_file(model_path, original_content),
			             "restore model after rollback-failure test");
		}
		{
			const auto original_content = read_file(model_path);
			std::latch clear_paused(1);
			std::latch allow_clear(1);
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .before_delete_commit = [&](const std::filesystem::path &) -> void {
				    clear_paused.count_down();
				    allow_clear.wait();
			    },
			});
			howdy::native::UserModelMutationResult                        clear_result;
			std::thread                                                   clearer([&] -> void {
				clear_result = howdy::native::clear_user_model_entries("alice");
			});
			clear_paused.wait();
			const auto during_clear =
			    howdy::native::list_user_model_entries("alice", backend, "cosine", "sface.onnx");
			allow_clear.count_down();
			clearer.join();
			const auto after_clear =
			    howdy::native::list_user_model_entries("alice", backend, "cosine", "sface.onnx");
			ok &= expect(during_clear.status == howdy::native::UserModelStatus::kOk &&
			                 !during_clear.entries.empty(),
			             "reader during clear observes original model document");
			ok &= expect(clear_result.status == howdy::native::UserModelStatus::kOk,
			             "concurrent-read clear succeeds");
			ok &= expect(after_clear.status == howdy::native::UserModelStatus::kNoModel,
			             "reader after clear observes no model");
			ok &= expect(write_file(model_path, original_content),
			             "restore model after concurrent-read clear test");
		}
		{
			const auto original_content = read_file(model_path);
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .fail_delete_unlink = true,
			});
			const auto result = howdy::native::clear_user_model_entries("alice");
			ok &= expect(result.status == howdy::native::UserModelStatus::kDeleteFailed,
			             "clear reports injected unlink failure");
			ok &= expect(read_file(model_path) == original_content,
			             "unlink failure leaves original model at canonical path");
		}
		{
			const auto original_content = read_file(model_path);
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .fail_parent_sync = true,
			});
			const auto result = howdy::native::clear_user_model_entries("alice");
			ok &= expect(result.status == howdy::native::UserModelStatus::kDurabilityUncertain,
			             "clear distinguishes committed parent-sync failure");
			ok &= expect(result.removed_last,
			             "clear parent-sync failure reports committed removal state");
			ok &= expect(!fs::exists(model_path),
			             "clear parent-sync failure leaves model removed from namespace");
			ok &= expect(write_file(model_path, original_content),
			             "restore model after clear parent-sync failure test");
		}
		when_supported(
		    geteuid() != 0,
		    [&] -> void {
			    const auto before_failed_writes = read_file(model_path);
			    ok &= expect(chmod(models_dir.c_str(), 0555) == 0,
			                 "make models directory unwritable for failed-write checks");
			    const auto append_result =
			        howdy::native::append_user_model_entry("alice", second_entry);
			    ok &= expect(append_result.status == howdy::native::UserModelStatus::kWriteFailed,
			                 "append reports atomic write failure");
			    ok &= expect(read_file(model_path) == before_failed_writes,
			                 "failed append write leaves model file unchanged");
			    const auto clear_result = howdy::native::clear_user_model_entries("alice");
			    ok &= expect(clear_result.status == howdy::native::UserModelStatus::kDeleteFailed,
			                 "clear reports failure when parent directory is unwritable");
			    ok &= expect(read_file(model_path) == before_failed_writes,
			                 "failed clear leaves model file unchanged");
			    ok &= expect(chmod(models_dir.c_str(), 0755) == 0,
			                 "restore models directory after failed-write checks");
		    },
		    "atomic write failure checks while running as root");

		{
			const auto result = howdy::native::remove_user_model_entry("alice", 99);
			ok &= expect(result.status == howdy::native::UserModelStatus::kModelNotFound,
			             "remove reports missing model ID");
		}
		{
			const auto listing =
			    howdy::native::list_user_model_entries("alice", backend, "cosine", "sface.onnx");
			ok &= expect(listing.status == howdy::native::UserModelStatus::kOk &&
			                 listing.entries.size() == 2,
			             "list entries before verified stale remove");
			const auto expected = expectation_from_entry(listing.entries[0]);
			ok &= expect(
			    write_file(
			        model_path,
			        R"([{"id":0,"time":2,"label":"changed","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.1,0.2]],"future_field":"preserved"},{"id":1,"time":1,"label":"second","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.3,0.4]]}])"),
			    "rewrite model entry after remove listing");
			const auto result =
			    howdy::native::remove_user_model_entry_if_matches("alice", expected);
			ok &= expect(result.status == howdy::native::UserModelStatus::kModelChanged,
			             "verified remove aborts when model entry changes after listing");
			const auto after =
			    howdy::native::list_user_model_entries("alice", backend, "cosine", "sface.onnx");
			ok &= expect(after.status == howdy::native::UserModelStatus::kOk &&
			                 after.entries.size() == 2 && after.entries[0].label == "changed",
			             "stale verified remove leaves changed model entry");
		}
		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":0,"time":1,"label":"first","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.1,0.2]],"future_field":"preserved"},{"id":1,"time":1,"label":"second","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.3,0.4]]}])"),
		    "restore unchanged entries before verified remove");
		{
			const auto listing =
			    howdy::native::list_user_model_entries("alice", backend, "cosine", "sface.onnx");
			ok &= expect(listing.status == howdy::native::UserModelStatus::kOk &&
			                 listing.entries.size() == 2,
			             "list entries before verified remove");
			const auto expected = expectation_from_entry(listing.entries[0]);
			const auto result =
			    howdy::native::remove_user_model_entry_if_matches("alice", expected);
			ok &=
			    expect(result.status == howdy::native::UserModelStatus::kOk && !result.removed_last,
			           "verified remove succeeds when model entry is unchanged");
			ok &= expect(result.entry.id == 0 && result.entry.label == "first",
			             "verified remove returns actual removed entry");
			const auto remaining = howdy::native::list_user_model_entries("alice", backend);
			ok &= expect(remaining.entries.size() == 1 && remaining.entries[0].id == 1,
			             "verified remove preserves other model entries");
		}
		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":0,"time":1,"label":"first","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.1,0.2]]},{"id":1,"time":1,"label":"second","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.3,0.4]],"future_field":{"revision":2}}])"),
		    "restore entries before legacy remove");
		{
			const auto result = howdy::native::remove_user_model_entry("alice", 0);
			ok &=
			    expect(result.status == howdy::native::UserModelStatus::kOk && !result.removed_last,
			           "remove deletes existing model ID");
			ok &= expect(result.entry.id == 0 && result.entry.label == "first",
			             "remove returns actual removed entry");
			const auto remaining = howdy::native::list_user_model_entries("alice", backend);
			ok &= expect(remaining.entries.size() == 1 && remaining.entries[0].id == 1,
			             "remove preserves other model entries");
			const auto persisted = read_file(model_path);
			ok &= expect(persisted.contains("\"future_field\"") &&
			                 persisted.contains("\"revision\":2"),
			             "remove preserves unknown fields in another entry");
		}
		{
			const auto result = howdy::native::remove_user_model_entry("alice", 1);
			ok &=
			    expect(result.status == howdy::native::UserModelStatus::kOk && result.removed_last,
			           "remove deletes last model entry");
			ok &= expect(!fs::exists(model_path), "remove last entry deletes model file");
		}

		ok &= expect(howdy::native::append_user_model_entry("alice", first_entry).status ==
		                 howdy::native::UserModelStatus::kOk,
		             "append recreates model before clear");
		{
			const auto result = howdy::native::clear_user_model_entries("alice");
			ok &= expect(result.status == howdy::native::UserModelStatus::kOk,
			             "clear removes all model entries");
			ok &= expect(!fs::exists(model_path), "clear deletes model file");
		}

		const auto created_models_dir = temp_root / "created-store-models";
		fs::remove_all(created_models_dir, ec);
		ec.clear();
		setenv("HOWDY_USER_MODELS_DIR", created_models_dir.c_str(), 1);
		{
			const auto result = howdy::native::append_user_model_entry("created-user", first_entry);
			ok &= expect(result.status == howdy::native::UserModelStatus::kOk,
			             "append creates missing secure models directory");
			struct stat dir_stat{};
			struct stat file_stat{};
			const auto  created_model_path = created_models_dir / "created-user.dat";
			ok &= expect(stat(created_models_dir.c_str(), &dir_stat) == 0,
			             "stat append-created models directory");
			ok &= expect(stat(created_model_path.c_str(), &file_stat) == 0,
			             "stat append-created model file");
			ok &= expect((dir_stat.st_mode & 0777) == 0750,
			             "append-created models directory uses 0750 mode");
			ok &= expect((file_stat.st_mode & 0777) == 0600,
			             "append-created model file uses 0600 mode");
		}
		fs::remove_all(created_models_dir, ec);
		ec.clear();
		setenv("HOWDY_USER_MODELS_DIR", models_dir.c_str(), 1);

		const auto stale_lock_path = fs::path(model_path.string() + ".lock");
		fs::remove(stale_lock_path, ec);
		ec.clear();
		when_supported(
		    symlink("/tmp", stale_lock_path.c_str()) == 0,
		    [&] -> void {
			    const auto result = howdy::native::append_user_model_entry("alice", first_entry);
			    ok &= expect(result.status == howdy::native::UserModelStatus::kOk,
			                 "stale sidecar lock symlink does not affect descriptor locking");
			    ok &= expect(fs::remove(stale_lock_path, ec), "remove stale lock symlink");
			    ec.clear();
		    },
		    "stale lock symlink creation failed");

		fs::remove_all(temp_root, ec);
		unsetenv("HOWDY_USER_MODELS_DIR");
		return ok;
	}

}  // namespace howdy::test::user_models
