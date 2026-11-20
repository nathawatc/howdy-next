#include "../../src/storage/user_model_codec/internal.hpp"
#include "../../src/storage/user_model_store/internal.hpp"
#include "storage/user_model_limits.hpp"
#include "storage/user_model_status.hpp"
#include "storage/user_model_store/test_hooks.hpp"
#include "storage/user_model_types.hpp"
#include "storage/user_models.hpp"
#include "storage/user_models_test_support.hpp"
#include "support/atomic_files.hpp"
#include "support/bit_flags.hpp"
#include "support/face_metric.hpp"
#include "test_support.hpp"

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

namespace howdy::test::user_models {

	namespace {

		// Runs an append in a child under an alarm, so a writer that waits on a lock fails
		// instead of hanging the suite.
		auto AppendsWithoutWaiting(const std::filesystem::path            &temp_root,
		                           const howdy::native::NewUserModelEntry &entry) -> bool {
			constexpr unsigned int write_alarm_seconds = 10;
			const pid_t            writer              = fork();
			if (writer == 0) {
				alarm(write_alarm_seconds);
				const auto appended =
				    howdy::native::AppendUserModelEntry("alice", entry, {temp_root});
				_exit(appended.status == howdy::native::UserModelStatus::kOk ? EXIT_SUCCESS
				                                                             : EXIT_FAILURE);
			}
			int status = 0;
			return writer > 0 && waitpid(writer, &status, 0) == writer && WIFEXITED(status) &&
			       WEXITSTATUS(status) == EXIT_SUCCESS;
		}

		auto ExpectRootOnlyNamespaceLock(const std::filesystem::path            &model_path,
		                                 const howdy::native::NewUserModelEntry &entry) -> bool {
			namespace fs       = std::filesystem;
			bool            ok = true;
			std::error_code ec;
			const auto      models_dir = model_path.parent_path();
			const auto      temp_root  = models_dir.parent_path();
			const auto      lock_path  = fs::path(models_dir.string() + ".lock");
			const auto      before     = ReadFile(model_path);
			{
				// The test models directory is 0755, so any user could open and flock it.
				const howdy::test::ScopedFd held(
				    open(models_dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
				ok &= Expect(held.Valid() && flock(held.Get(), LOCK_SH | LOCK_NB) == 0,
				             "hold a shared lock on the models directory, as a user could");
				ok &= Expect(AppendsWithoutWaiting(temp_root, entry),
				             "a user's lock on the models directory does not stall a writer");
			}
			struct stat lock_stat{};
			ok &= Expect(lstat(lock_path.c_str(), &lock_stat) == 0 && S_ISREG(lock_stat.st_mode) &&
			                 howdy::native::MaskedBits(lock_stat.st_mode, 07777) == 0600,
			             "writers lock a 0600 file beside the models directory");

			const auto expect_rejected = [&](std::string_view description) -> void {
				const auto current = ReadFile(model_path);
				const auto result =
				    howdy::native::AppendUserModelEntry("alice", entry, {temp_root});
				ok &= Expect(result.status == howdy::native::UserModelStatus::kInsecurePath &&
				                 result.error_message.contains("User model lock file") &&
				                 result.error_message.contains(lock_path.string()),
				             std::string(description) + ": reported as an insecure lock file");
				ok &= Expect(ReadFile(model_path) == current,
				             std::string(description) + ": model bytes unchanged");
			};
			ok &= Expect(chmod(lock_path.c_str(), 0640) == 0, "make the lock file group-readable");
			expect_rejected("a lock file other users can open is rejected");
			ok &= Expect(chmod(lock_path.c_str(), 0600) == 0, "restore the lock file mode");

			const auto lock_link = fs::path(lock_path.string() + "-link");
			ok &=
			    Expect(link(lock_path.c_str(), lock_link.c_str()) == 0, "hard-link the lock file");
			expect_rejected("a hard-linked lock file is rejected");
			ok &= Expect(unlink(lock_link.c_str()) == 0, "remove the lock file hard link");

			const auto victim_path    = temp_root / "lock-symlink-victim.txt";
			const auto victim_content = std::string("lock victim sentinel\n");
			ok &= Expect(WriteFile(victim_path, victim_content) && unlink(lock_path.c_str()) == 0 &&
			                 symlink(victim_path.c_str(), lock_path.c_str()) == 0,
			             "replace the lock file with a symlink");
			expect_rejected("a symlinked lock file is rejected");
			ok &= Expect(ReadFile(victim_path) == victim_content,
			             "a symlinked lock file leaves its target unchanged");
			ok &= Expect(unlink(lock_path.c_str()) == 0, "remove the lock file symlink");
			fs::remove(victim_path, ec);

			// Later tests expect the model as it was before these appends.
			ok &= Expect(WriteFile(model_path, before) && chmod(model_path.c_str(), 0600) == 0,
			             "restore the model after the lock file checks");
			return ok;
		}

		// The open reports what failed, not a lock failure. The before_lock hook runs after the
		// path checks and before the open, where only the open itself can notice a change.
		auto ExpectOpenFailureCauses(const std::filesystem::path            &model_path,
		                             const howdy::native::NewUserModelEntry &entry) -> bool {
			namespace fs = std::filesystem;
			using howdy::native::UserModelStatus;
			using howdy::native::UserModelStore;
			using howdy::native::user_model_store_test_hooks::ScopedHooks;

			struct Outcome {
				UserModelStatus status = UserModelStatus::kOk;
				std::string     message;
			};

			bool            ok = true;
			std::error_code ec;
			const auto      temp_root = model_path.parent_path().parent_path();
			const auto      before    = ReadFile(model_path);
			const auto      restore   = [&](const std::string &description) -> void {
				fs::remove(model_path, ec);
				ok &= Expect(WriteFile(model_path, before) && chmod(model_path.c_str(), 0600) == 0,
				             "restore the model after " + description);
			};
			const auto begin_mutation = [&] -> Outcome {
				auto        result = UserModelStore::BeginMutation("alice", {temp_root});
				const auto &listed = result.document.Result();
				return {.status = listed.status, .message = listed.error_message};
			};
			const auto lock_existing = [&] -> Outcome {
				const auto result = UserModelStore::LockExisting("alice", {temp_root});
				return {.status = result.status, .message = result.error_message};
			};

			const auto victim_path    = temp_root / "open-symlink-victim.txt";
			const auto victim_content = std::string("open victim sentinel\n");
			ok &= Expect(WriteFile(victim_path, victim_content), "write the open victim");
			const auto expect_symlink_rejected = [&](const std::string &name,
			                                         const auto        &run) -> void {
				PostLockSymlinkSwap hook{.victim_path = victim_path};
				{
					const ScopedHooks hooks({
					    .before_lock = [&hook](const fs::path &path) -> void {
						    ReplaceWithVictimSymlinkAfterLock(&hook, path);
					    },
					});
					const auto        outcome = run();
					ok &= Expect(hook.swapped, name + ": swap in a symlink before the open");
					ok &= Expect(outcome.status == UserModelStatus::kInsecurePath &&
					                 outcome.message.contains("must be a regular file"),
					             name + ": a symlink at the open is an insecure path");
				}
				ok &= Expect(ReadFile(victim_path) == victim_content,
				             name + ": a symlink at the open leaves its target unchanged");
				restore(name + " symlink check");
			};
			expect_symlink_rejected("begin_mutation", begin_mutation);
			expect_symlink_rejected("lock_existing", lock_existing);
			fs::remove(victim_path, ec);

			{
				const ScopedHooks hooks({
				    .before_lock = [&](const fs::path &path) -> void {
					    ok &= Expect(chmod(path.c_str(), 0620) == 0,
						             "make the model group-writable before the open");
				    },
				});
				const auto        outcome = begin_mutation();
				ok &= Expect(outcome.status == UserModelStatus::kInsecurePath &&
				                 outcome.message.contains("failed security validation"),
				             "a group-writable model at the open is an insecure path");
			}
			restore("group-writable check");

			const auto remove_model = [&](const fs::path &path) -> void {
				fs::remove(path, ec);
			};
			{
				const ScopedHooks hooks({.before_lock = remove_model});
				ok &= Expect(lock_existing().status == UserModelStatus::kNoModel,
				             "lock_existing reports a model removed before the open as no model");
			}
			restore("removed-model lock check");
			{
				const auto inspection = howdy::native::InspectUserModelFile("alice", {temp_root});
				const ScopedHooks hooks({.before_lock = remove_model});
				ok &= Expect(inspection.snapshot.has_value() &&
				                 howdy::native::ClearUserModelEntriesIfUnchanged(
				                     "alice", *inspection.snapshot, {temp_root})
				                         .status == UserModelStatus::kModelChanged,
				             "clear reports a model removed before the open as changed");
			}
			restore("removed-model clear check");
			{
				const ScopedHooks hooks({.before_lock = remove_model});
				const auto        appended =
				    howdy::native::AppendUserModelEntry("alice", entry, {temp_root});
				const auto listed = howdy::native::ListUserModelEntries("alice", entry.backend,
				                                                        entry.model, {temp_root});
				ok &=
				    Expect(appended.status == UserModelStatus::kOk &&
					           listed.status == UserModelStatus::kOk && listed.entries.size() == 1,
					       "an append whose model was removed before the open creates one");
			}
			restore("removed-model append check");

			WhenSupported(
			    geteuid() != 0,
			    [&] -> void {
				    {
					    const ScopedHooks hooks({
					        .before_lock = [&](const fs::path &path) -> void {
						        ok &= Expect(chmod(path.c_str(), 0) == 0,
								             "make the model unreadable before the open");
					        },
					    });
					    const auto        outcome = begin_mutation();
					    ok &=
					        Expect(outcome.status == UserModelStatus::kLockFailed &&
							           outcome.message.contains("Failed to open user model file") &&
							           outcome.message.contains(model_path.string()),
							       "an unexpected open failure names the model file and errno");
				    }
				    restore("unreadable-model check");
			    },
			    "open permission failure while running as root");
			return ok;
		}

		// Anything put at a free model path between the open and the commit is a taken path,
		// however late it appears: staging refuses a symlink there, and RENAME_NOREPLACE refuses
		// whatever is there once staged.
		auto ExpectFirstModelPathTaken(const std::filesystem::path &model_path) -> bool {
			namespace fs = std::filesystem;
			using howdy::native::AtomicFileCommitResult;
			using howdy::native::user_model_store_test_hooks::ScopedHooks;

			bool            ok = true;
			std::error_code ec;
			const auto      models_dir     = model_path.parent_path();
			const auto      temp_root      = models_dir.parent_path();
			const auto      first_path     = models_dir / "bob.dat";
			const auto      victim_path    = temp_root / "first-model-symlink-victim.txt";
			const auto      victim_content = std::string("first model victim sentinel\n");
			const auto      taker_content  = std::string("not a model\n");
			const auto      put_symlink    = [&](const fs::path &path) -> bool {
				return symlink(victim_path.c_str(), path.c_str()) == 0;
			};
			const auto put_file = [&](const fs::path &path) -> bool {
				return WriteFile(path, taker_content);
			};
			const auto expect_taken = [&](const std::string &name, bool after_staging,
			                              const std::function<bool(const fs::path &)> &take,
			                              const std::function<bool()> &left_alone) -> void {
				fs::remove(first_path, ec);
				ok &= Expect(WriteFile(victim_path, victim_content), name + ": write the victim");
				{
					auto mutation =
					    howdy::native::UserModelStore::BeginMutation("bob", {temp_root});
					ok &= Expect(mutation.transaction.has_value() &&
					                 mutation.document.Result().status ==
					                     howdy::native::UserModelStatus::kNoModel,
					             name + ": begin a first enrollment");
					ok &= Expect(howdy::native::user_model_codec::AppendEntry(
					                 mutation.document,
					                 {
					                     .id        = 0,
					                     .time      = 1,
					                     .label     = "first",
					                     .encodings = {{0.1F, 0.2F}},
					                 }),
					             name + ": add the first entry");
					bool taken = false;
					if (!after_staging) {
						taken = take(first_path);
					}
					const ScopedHooks hooks({
					    .before_write_commit = [&](const fs::path &path) -> void {
						    if (after_staging) {
							    taken = take(path);
						    }
					    },
					});
					const auto result = mutation.transaction.has_value()
					                        ? mutation.transaction->WriteDocument(mutation.document)
					                        : AtomicFileCommitResult::kNotCommitted;
					ok &= Expect(taken, name + ": take the free model path after the open");
					ok &= Expect(result == AtomicFileCommitResult::kDestinationExists,
					             name + ": a first model refuses a taken path");
				}
				ok &= Expect(left_alone() && ReadFile(victim_path) == victim_content,
				             name + ": a refused first model leaves the path as it was");
				ok &= Expect(StagedUserModelPaths(models_dir).empty(),
				             name + ": a refused first model leaves nothing staged");
				fs::remove(first_path, ec);
				fs::remove(victim_path, ec);
			};
			const auto symlink_left = [&] -> bool {
				struct stat path_stat{};
				return lstat(first_path.c_str(), &path_stat) == 0 && S_ISLNK(path_stat.st_mode);
			};
			const auto file_left = [&] -> bool {
				return ReadFile(first_path) == taker_content;
			};
			expect_taken("symlink before staging", false, put_symlink, symlink_left);
			expect_taken("symlink after staging", true, put_symlink, symlink_left);
			expect_taken("regular file after staging", true, put_file, file_left);
			return ok;
		}

	}  // namespace

	auto TestUserModelMutationStart() -> bool {
		namespace fs                 = std::filesystem;
		bool              ok         = true;
		const auto        temp_root  = fs::current_path() / "howdy-user-models-test";
		const auto        models_dir = temp_root / "models";
		const auto        model_path = models_dir / "alice.dat";
		const std::string backend    = "opencv_dnn_sface";
		std::error_code   ec;

		fs::remove(model_path, ec);
		ec.clear();
		const howdy::native::NewUserModelEntry first_entry{
		    .label     = "first",
		    .backend   = backend,
		    .metric    = howdy::native::FaceMetric::kCosine,
		    .model     = "sface.onnx",
		    .encodings = {{0.1F, 0.2F}},
		};
		const howdy::native::UserModelEntryExpectation first_entry_expectation{
		    .id      = 0,
		    .time    = 1,
		    .label   = "first",
		    .backend = backend,
		    .metric  = howdy::native::FaceMetric::kCosine,
		    .model   = "sface.onnx",
		};
		{
			const std::string original_model =
			    R"([{"id":0,"time":1,"label":"first","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.1,0.2]]}])";
			const auto victim_path    = temp_root / "post-lock-revalidate-victim.txt";
			const auto victim_content = std::string("victim sentinel\n");
			ok &= Expect(
			    WriteFile(model_path, original_model),
			    "write secure existing model before begin-mutation post-lock revalidation test");
			ok &= Expect(WriteFile(victim_path, victim_content),
			             "write victim file before begin-mutation post-lock revalidation test");

			PostLockSymlinkSwap hook{.victim_path = victim_path};
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .after_lock_before_revalidate = [&hook](const std::filesystem::path &path) -> void {
				    ReplaceWithVictimSymlinkAfterLock(&hook, path);
			    },
			});
			const auto result = howdy::native::UserModelStore::BeginMutation("alice", {temp_root});
			ok &= Expect(hook.calls == 1,
			             "begin_mutation post-lock revalidation hook executes exactly once");
			ok &= Expect(hook.swapped,
			             "begin_mutation post-lock hook atomically replaces model with symlink");
			ok &= Expect(result.document.Result().status ==
			                 howdy::native::UserModelStatus::kInsecurePath,
			             "begin_mutation post-lock revalidation rejects symlink replacement");
			ok &= Expect(!result.transaction.has_value(),
			             "begin_mutation post-lock validation failure creates no transaction");
			ok &= Expect(
			    ReadFile(victim_path) == victim_content,
			    "begin_mutation post-lock validation failure leaves victim content unchanged");
			struct stat replaced_stat{};
			ok &= Expect(
			    lstat(model_path.c_str(), &replaced_stat) == 0 && S_ISLNK(replaced_stat.st_mode),
			    "begin_mutation post-lock test replacement remains symlink after failed mutation");
			fs::remove(model_path, ec);
			ec.clear();
			fs::remove(victim_path, ec);
			ec.clear();
		}
		{
			const std::string original_model =
			    R"([{"id":0,"time":1,"label":"first","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.1,0.2]]}])";
			const auto victim_path    = temp_root / "post-lock-lock-existing-victim.txt";
			const auto victim_content = std::string("lock existing victim sentinel\n");
			ok &= Expect(
			    WriteFile(model_path, original_model),
			    "write secure existing model before lock_existing post-lock revalidation test");
			ok &= Expect(WriteFile(victim_path, victim_content),
			             "write victim file before lock_existing post-lock revalidation test");

			PostLockSymlinkSwap hook{.victim_path = victim_path};
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .after_lock_before_revalidate = [&hook](const std::filesystem::path &path) -> void {
				    ReplaceWithVictimSymlinkAfterLock(&hook, path);
			    },
			});
			const auto result = howdy::native::UserModelStore::LockExisting("alice", {temp_root});
			ok &= Expect(hook.calls == 1,
			             "lock_existing post-lock revalidation hook executes exactly once");
			ok &= Expect(hook.swapped,
			             "lock_existing post-lock hook atomically replaces model with symlink");
			ok &= Expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
			             "lock_existing post-lock revalidation rejects symlink replacement");
			ok &= Expect(!result.transaction.has_value(),
			             "lock_existing post-lock validation failure creates no transaction");
			ok &= Expect(
			    ReadFile(victim_path) == victim_content,
			    "lock_existing post-lock validation failure leaves victim content unchanged");
			struct stat replaced_stat{};
			ok &= Expect(
			    lstat(model_path.c_str(), &replaced_stat) == 0 && S_ISLNK(replaced_stat.st_mode),
			    "lock_existing post-lock test replacement remains symlink after failed mutation");
			fs::remove(model_path, ec);
			ec.clear();
			fs::remove(victim_path, ec);
			ec.clear();
		}
		{
			const std::string original_model =
			    R"([{"id":0,"time":1,"label":"first","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.1,0.2]]}])";
			const std::string replacement_model =
			    R"([{"id":0,"time":1,"label":"replacement","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.5,0.6]]}])";
			const auto replacement_path = temp_root / "post-lock-begin-mutation-replacement.dat";
			ok &=
			    Expect(WriteFile(model_path, original_model),
				       "write secure existing model before begin_mutation regular-file swap test");
			ok &= Expect(WriteFile(replacement_path, replacement_model),
			             "write replacement file before begin_mutation regular-file swap test");

			PostLockRegularFileSwap hook{.replacement_path = replacement_path};
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .after_lock_before_revalidate = [&hook](const std::filesystem::path &path) -> void {
				    ReplaceWithRegularFileAfterLock(&hook, path);
			    },
			});
			const auto result = howdy::native::UserModelStore::BeginMutation("alice", {temp_root});
			ok &= Expect(hook.calls == 1,
			             "begin_mutation regular-file swap hook executes exactly once");
			ok &= Expect(hook.swapped,
			             "begin_mutation hook atomically replaces model with regular file");
			ok &= Expect(result.document.Result().status ==
			                 howdy::native::UserModelStatus::kModelChanged,
			             "begin_mutation rejects post-lock regular-file inode replacement");
			ok &= Expect(!result.transaction.has_value(),
			             "begin_mutation regular-file replacement failure creates no transaction");
			ok &= Expect(ReadFile(model_path) == replacement_model,
			             "begin_mutation regular-file replacement remains unmodified");
			fs::remove(model_path, ec);
			ec.clear();
		}
		{
			const std::string original_model =
			    R"([{"id":0,"time":1,"label":"first","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.1,0.2]]}])";
			const std::string replacement_model =
			    R"([{"id":0,"time":1,"label":"replacement","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.7,0.8]]}])";
			const auto replacement_path = temp_root / "post-lock-lock-existing-replacement.dat";
			ok &= Expect(WriteFile(model_path, original_model),
			             "write secure existing model before lock_existing regular-file swap test");
			ok &= Expect(WriteFile(replacement_path, replacement_model),
			             "write replacement file before lock_existing regular-file swap test");

			PostLockRegularFileSwap hook{.replacement_path = replacement_path};
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .after_lock_before_revalidate = [&hook](const std::filesystem::path &path) -> void {
				    ReplaceWithRegularFileAfterLock(&hook, path);
			    },
			});
			const auto result = howdy::native::UserModelStore::LockExisting("alice", {temp_root});
			ok &= Expect(hook.calls == 1,
			             "lock_existing regular-file swap hook executes exactly once");
			ok &= Expect(hook.swapped,
			             "lock_existing hook atomically replaces model with regular file");
			ok &= Expect(result.status == howdy::native::UserModelStatus::kModelChanged,
			             "lock_existing rejects post-lock regular-file inode replacement");
			ok &= Expect(!result.transaction.has_value(),
			             "lock_existing regular-file replacement failure creates no transaction");
			ok &= Expect(ReadFile(model_path) == replacement_model,
			             "lock_existing regular-file replacement remains unmodified");
			fs::remove(model_path, ec);
			ec.clear();
		}
		{
			const std::string original_model =
			    R"([{"id":0,"time":1,"label":"first","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.1,0.2]]}])";
			const std::string replacement_model =
			    R"([{"id":0,"time":1,"label":"aba-replacement","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.9,1.0]]}])";
			AbaRegularFileSwap hook{
			    .replacement_path              = temp_root / "begin-mutation-aba-b.dat",
			    .original_saved_path           = temp_root / "begin-mutation-aba-a-saved.dat",
			    .locked_replacement_saved_path = temp_root / "begin-mutation-aba-b-locked.dat",
			};
			ok &= Expect(WriteFile(model_path, original_model),
			             "write A before begin_mutation ABA swap test");
			ok &= Expect(WriteFile(hook.replacement_path, replacement_model),
			             "write B before begin_mutation ABA swap test");
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .before_lock = [&hook](const std::filesystem::path &path) -> void {
				    InstallReplacementBeforeLock(&hook, path);
			    },
			    .after_lock_before_revalidate = [&hook](const std::filesystem::path &path) -> void {
				    RestoreOriginalAfterLock(&hook, path);
			    },
			});
			const auto result = howdy::native::UserModelStore::BeginMutation("alice", {temp_root});
			ok &= Expect(hook.before_lock_calls == 1 && hook.after_lock_calls == 1,
			             "begin_mutation ABA hooks execute exactly once");
			ok &=
			    Expect(hook.installed_b && hook.restored_a && !hook.rollback_failed,
				       "begin_mutation ABA test installs B before lock and restores A after lock");
			ok &= Expect(result.document.Result().status ==
			                 howdy::native::UserModelStatus::kModelChanged,
			             "begin_mutation rejects ABA inode replacement");
			ok &= Expect(!result.transaction.has_value(),
			             "begin_mutation ABA rejection creates no transaction");
			ok &= Expect(ReadFile(model_path) == original_model,
			             "begin_mutation ABA restored original remains unmodified");
			ok &= Expect(ReadFile(hook.locked_replacement_saved_path) == replacement_model,
			             "begin_mutation ABA locked replacement remains unmodified");
			fs::remove(model_path, ec);
			ec.clear();
			fs::remove(hook.locked_replacement_saved_path, ec);
			ec.clear();
		}
		{
			const std::string original_model =
			    R"([{"id":0,"time":1,"label":"first","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.1,0.2]]}])";
			const std::string replacement_model =
			    R"([{"id":0,"time":1,"label":"aba-replacement","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[1.1,1.2]]}])";
			AbaRegularFileSwap hook{
			    .replacement_path              = temp_root / "lock-existing-aba-b.dat",
			    .original_saved_path           = temp_root / "lock-existing-aba-a-saved.dat",
			    .locked_replacement_saved_path = temp_root / "lock-existing-aba-b-locked.dat",
			};
			ok &= Expect(WriteFile(model_path, original_model),
			             "write A before lock_existing ABA swap test");
			ok &= Expect(WriteFile(hook.replacement_path, replacement_model),
			             "write B before lock_existing ABA swap test");
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .before_lock = [&hook](const std::filesystem::path &path) -> void {
				    InstallReplacementBeforeLock(&hook, path);
			    },
			    .after_lock_before_revalidate = [&hook](const std::filesystem::path &path) -> void {
				    RestoreOriginalAfterLock(&hook, path);
			    },
			});
			const auto result = howdy::native::UserModelStore::LockExisting("alice", {temp_root});
			ok &= Expect(hook.before_lock_calls == 1 && hook.after_lock_calls == 1,
			             "lock_existing ABA hooks execute exactly once");
			ok &= Expect(hook.installed_b && hook.restored_a && !hook.rollback_failed,
			             "lock_existing ABA test installs B before lock and restores A after lock");
			ok &= Expect(result.status == howdy::native::UserModelStatus::kModelChanged,
			             "lock_existing rejects ABA inode replacement");
			ok &= Expect(!result.transaction.has_value(),
			             "lock_existing ABA rejection creates no transaction");
			ok &= Expect(ReadFile(model_path) == original_model,
			             "lock_existing ABA restored original remains unmodified");
			ok &= Expect(ReadFile(hook.locked_replacement_saved_path) == replacement_model,
			             "lock_existing ABA locked replacement remains unmodified");
			fs::remove(model_path, ec);
			ec.clear();
			fs::remove(hook.locked_replacement_saved_path, ec);
			ec.clear();
		}
		ok &= Expect(WriteFile(model_path, "not-json"),
		             "write malformed existing file before mutations");
		const auto malformed_before_mutation = ReadFile(model_path);
		{
			const auto result =
			    howdy::native::AppendUserModelEntry("alice", first_entry, {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kParseError,
			             "append rejects malformed existing file");
			ok &= Expect(ReadFile(model_path) == malformed_before_mutation,
			             "failed append leaves malformed existing file unchanged");
		}
		{
			const auto result = howdy::native::RemoveUserModelEntryIfMatches(
			    "alice", first_entry_expectation, {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kParseError,
			             "remove rejects malformed existing file");
			ok &= Expect(ReadFile(model_path) == malformed_before_mutation,
			             "failed remove leaves malformed existing file unchanged");
		}
		ok &= Expect(WriteFile(model_path, R"({"id":1})"),
		             "write invalid-shape existing file before mutations");
		const auto invalid_shape_before_mutation = ReadFile(model_path);
		{
			const auto result =
			    howdy::native::AppendUserModelEntry("alice", first_entry, {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "append rejects invalid-shape existing file");
			ok &= Expect(ReadFile(model_path) == invalid_shape_before_mutation,
			             "failed append leaves invalid-shape existing file unchanged");
		}
		{
			const auto result = howdy::native::RemoveUserModelEntryIfMatches(
			    "alice", first_entry_expectation, {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "remove rejects invalid-shape existing file");
			ok &= Expect(ReadFile(model_path) == invalid_shape_before_mutation,
			             "failed remove leaves invalid-shape existing file unchanged");
		}
		const auto deeply_nested_model =
		    R"([{"id":0,"time":1,"label":"deep","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.1]],"unknown":)" +
		    NestedArray(howdy::native::user_model_limits::kMaxJsonNestingDepth) + "}]";
		ok &= Expect(WriteFile(model_path, deeply_nested_model),
		             "write deeply nested unknown field before mutations");
		const auto deeply_nested_before_mutation = ReadFile(model_path);
		{
			const auto result =
			    howdy::native::AppendUserModelEntry("alice", first_entry, {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kOversized,
			             "append rejects deeply nested unknown field");
			ok &= Expect(ReadFile(model_path) == deeply_nested_before_mutation,
			             "deep nesting append failure leaves model file unchanged");
		}
		{
			const auto result = howdy::native::RemoveUserModelEntryIfMatches(
			    "alice", first_entry_expectation, {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kOversized,
			             "remove rejects deeply nested unknown field");
			ok &= Expect(ReadFile(model_path) == deeply_nested_before_mutation,
			             "deep nesting remove failure leaves model file unchanged");
		}
		fs::remove(model_path, ec);
		ec.clear();
		const auto first_append =
		    howdy::native::AppendUserModelEntry("alice", first_entry, {temp_root});
		ok &= Expect(first_append.status == howdy::native::UserModelStatus::kOk,
		             "append creates first model entry");
		ok &= Expect(first_append.entry.id == 0, "append allocates first model ID");
		{
			const auto                             before_oversized_append = ReadFile(model_path);
			const howdy::native::NewUserModelEntry oversized_entry{
			    .label = std::string(static_cast<std::size_t>(
			                             howdy::native::user_model_limits::kMaxUserModelFileBytes),
				                     'x'),
			    .backend   = backend,
			    .metric    = howdy::native::FaceMetric::kCosine,
			    .model     = "sface.onnx",
			    .encodings = {{0.3F, 0.4F}},
			};
			const auto result =
			    howdy::native::AppendUserModelEntry("alice", oversized_entry, {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kWriteFailed,
			             "append rejects serialized model larger than read limit");
			ok &= Expect(ReadFile(model_path) == before_oversized_append,
			             "oversized append leaves existing model file unchanged");
		}
		ok &= Expect(
		    WriteFile(
		        model_path,
		        R"([{"id":0,"time":1,"label":"first","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.1,0.2]],"future_field":"preserved"}])"),
		    "add unknown field to existing model entry");

		const howdy::native::NewUserModelEntry second_entry{
		    .label     = "second",
		    .backend   = backend,
		    .metric    = howdy::native::FaceMetric::kCosine,
		    .model     = "sface.onnx",
		    .encodings = {{0.3F, 0.4F}},
		};
		const howdy::native::NewUserModelEntry invalid_encoding_entry{
		    .label     = "invalid",
		    .backend   = backend,
		    .metric    = howdy::native::FaceMetric::kCosine,
		    .model     = "sface.onnx",
		    .encodings = {{std::numeric_limits<float>::infinity()}},
		};
		{
			const auto result =
			    howdy::native::AppendUserModelEntry("alice", invalid_encoding_entry, {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "append rejects invalid new-entry encodings before writing");
			std::ifstream     persisted(model_path);
			const std::string persisted_text((std::istreambuf_iterator<char>(persisted)),
			                                 std::istreambuf_iterator<char>());
			ok &= Expect(!persisted_text.contains("invalid"),
			             "append does not write invalid new-entry encodings");
		}
		const auto second_append =
		    howdy::native::AppendUserModelEntry("alice", second_entry, {temp_root});
		ok &= Expect(second_append.status == howdy::native::UserModelStatus::kOk,
		             "append adds second model entry");
		ok &= Expect(second_append.entry.id == 1, "append allocates next model ID");
		{
			const auto result =
			    howdy::native::ListUserModelEntries("alice", backend, "sface.onnx", {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kOk,
			             "lifecycle listing loads appended entries");
			ok &= Expect(result.entries.size() == 2, "append preserves existing entries");
			ok &= Expect(result.entries.at(0).label == "first" &&
			                 result.entries.at(1).label == "second",
			             "lifecycle listing preserves entry order");
			ok &= Expect(result.next_id == 2, "lifecycle listing reports next model ID");
			std::ifstream     persisted(model_path);
			const std::string persisted_text((std::istreambuf_iterator<char>(persisted)),
			                                 std::istreambuf_iterator<char>());
			ok &= Expect(persisted_text.contains("future_field"),
			             "append preserves unknown fields in existing entries");
		}
		{
			// The auth helper hands a user a read-only descriptor to their model, which the user
			// may flock and keep. Writers must not wait for that: they lock only a root-only file.
			const auto                  before = howdy::test::ReadFile(model_path);
			const howdy::test::ScopedFd held(open(model_path.c_str(), O_RDONLY | O_CLOEXEC));
			ok &= Expect(held.Valid() && flock(held.Get(), LOCK_SH | LOCK_NB) == 0,
			             "hold a shared lock on the model file, as a user could");
			ok &= Expect(AppendsWithoutWaiting(temp_root, second_entry),
			             "a user's lock on the model file does not stall a writer");
			// Later tests expect the two entries above.
			ok &= Expect(WriteFile(model_path, before) && chmod(model_path.c_str(), 0600) == 0,
			             "restore the model after the locked write");
		}
		ok &= ExpectRootOnlyNamespaceLock(model_path, second_entry);
		ok &= ExpectOpenFailureCauses(model_path, second_entry);
		ok &= ExpectFirstModelPathTaken(model_path);
		return ok;
	}
}  // namespace howdy::test::user_models
