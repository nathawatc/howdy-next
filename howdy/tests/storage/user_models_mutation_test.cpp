#include "storage/user_model_limits.hpp"
#include "storage/user_model_store.hpp"
#include "storage/user_model_store_test_hooks.hpp"
#include "storage/user_models_test_support.hpp"

namespace howdy::test::user_models {
	using namespace howdy::test::user_models;

	auto test_user_model_mutation_start() -> bool {
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
		    .metric    = "cosine",
		    .model     = "sface.onnx",
		    .encodings = {{0.1F, 0.2F}},
		};
		{
			const std::string original_model =
			    R"([{"id":0,"time":1,"label":"first","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.1,0.2]]}])";
			const auto victim_path    = temp_root / "post-lock-revalidate-victim.txt";
			const auto victim_content = std::string("victim sentinel\n");
			ok &= expect(
			    write_file(model_path, original_model),
			    "write secure existing model before begin-mutation post-lock revalidation test");
			ok &= expect(write_file(victim_path, victim_content),
			             "write victim file before begin-mutation post-lock revalidation test");

			PostLockSymlinkSwap hook{.victim_path = victim_path};
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .after_lock_before_revalidate = [&hook](const std::filesystem::path &path) -> void {
				    replace_with_victim_symlink_after_lock(&hook, path);
			    },
			});
			const auto result = howdy::native::UserModelStore::begin_mutation("alice");
			ok &= expect(hook.calls == 1,
			             "begin_mutation post-lock revalidation hook executes exactly once");
			ok &= expect(hook.swapped,
			             "begin_mutation post-lock hook atomically replaces model with symlink");
			ok &= expect(result.document.result.status ==
			                 howdy::native::UserModelStatus::kInsecurePath,
			             "begin_mutation post-lock revalidation rejects symlink replacement");
			ok &= expect(!result.transaction.has_value(),
			             "begin_mutation post-lock validation failure creates no transaction");
			ok &= expect(
			    read_file(victim_path) == victim_content,
			    "begin_mutation post-lock validation failure leaves victim content unchanged");
			struct stat replaced_stat{};
			ok &= expect(
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
			ok &= expect(
			    write_file(model_path, original_model),
			    "write secure existing model before lock_existing post-lock revalidation test");
			ok &= expect(write_file(victim_path, victim_content),
			             "write victim file before lock_existing post-lock revalidation test");

			PostLockSymlinkSwap hook{.victim_path = victim_path};
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .after_lock_before_revalidate = [&hook](const std::filesystem::path &path) -> void {
				    replace_with_victim_symlink_after_lock(&hook, path);
			    },
			});
			const auto result = howdy::native::UserModelStore::lock_existing("alice");
			ok &= expect(hook.calls == 1,
			             "lock_existing post-lock revalidation hook executes exactly once");
			ok &= expect(hook.swapped,
			             "lock_existing post-lock hook atomically replaces model with symlink");
			ok &= expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
			             "lock_existing post-lock revalidation rejects symlink replacement");
			ok &= expect(!result.transaction.has_value(),
			             "lock_existing post-lock validation failure creates no transaction");
			ok &= expect(
			    read_file(victim_path) == victim_content,
			    "lock_existing post-lock validation failure leaves victim content unchanged");
			struct stat replaced_stat{};
			ok &= expect(
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
			    expect(write_file(model_path, original_model),
			           "write secure existing model before begin_mutation regular-file swap test");
			ok &= expect(write_file(replacement_path, replacement_model),
			             "write replacement file before begin_mutation regular-file swap test");

			PostLockRegularFileSwap hook{.replacement_path = replacement_path};
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .after_lock_before_revalidate = [&hook](const std::filesystem::path &path) -> void {
				    replace_with_regular_file_after_lock(&hook, path);
			    },
			});
			const auto result = howdy::native::UserModelStore::begin_mutation("alice");
			ok &= expect(hook.calls == 1,
			             "begin_mutation regular-file swap hook executes exactly once");
			ok &= expect(hook.swapped,
			             "begin_mutation hook atomically replaces model with regular file");
			ok &= expect(result.document.result.status ==
			                 howdy::native::UserModelStatus::kModelChanged,
			             "begin_mutation rejects post-lock regular-file inode replacement");
			ok &= expect(!result.transaction.has_value(),
			             "begin_mutation regular-file replacement failure creates no transaction");
			ok &= expect(read_file(model_path) == replacement_model,
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
			ok &= expect(write_file(model_path, original_model),
			             "write secure existing model before lock_existing regular-file swap test");
			ok &= expect(write_file(replacement_path, replacement_model),
			             "write replacement file before lock_existing regular-file swap test");

			PostLockRegularFileSwap hook{.replacement_path = replacement_path};
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .after_lock_before_revalidate = [&hook](const std::filesystem::path &path) -> void {
				    replace_with_regular_file_after_lock(&hook, path);
			    },
			});
			const auto result = howdy::native::UserModelStore::lock_existing("alice");
			ok &= expect(hook.calls == 1,
			             "lock_existing regular-file swap hook executes exactly once");
			ok &= expect(hook.swapped,
			             "lock_existing hook atomically replaces model with regular file");
			ok &= expect(result.status == howdy::native::UserModelStatus::kModelChanged,
			             "lock_existing rejects post-lock regular-file inode replacement");
			ok &= expect(!result.transaction.has_value(),
			             "lock_existing regular-file replacement failure creates no transaction");
			ok &= expect(read_file(model_path) == replacement_model,
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
			ok &= expect(write_file(model_path, original_model),
			             "write A before begin_mutation ABA swap test");
			ok &= expect(write_file(hook.replacement_path, replacement_model),
			             "write B before begin_mutation ABA swap test");
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .before_lock = [&hook](const std::filesystem::path &path) -> void {
				    install_replacement_before_lock(&hook, path);
			    },
			    .after_lock_before_revalidate = [&hook](const std::filesystem::path &path) -> void {
				    restore_original_after_lock(&hook, path);
			    },
			});
			const auto result = howdy::native::UserModelStore::begin_mutation("alice");
			ok &= expect(hook.before_lock_calls == 1 && hook.after_lock_calls == 1,
			             "begin_mutation ABA hooks execute exactly once");
			ok &=
			    expect(hook.installed_b && hook.restored_a,
			           "begin_mutation ABA test installs B before lock and restores A after lock");
			ok &= expect(result.document.result.status ==
			                 howdy::native::UserModelStatus::kModelChanged,
			             "begin_mutation rejects ABA inode replacement");
			ok &= expect(!result.transaction.has_value(),
			             "begin_mutation ABA rejection creates no transaction");
			ok &= expect(read_file(model_path) == original_model,
			             "begin_mutation ABA restored original remains unmodified");
			ok &= expect(read_file(hook.locked_replacement_saved_path) == replacement_model,
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
			ok &= expect(write_file(model_path, original_model),
			             "write A before lock_existing ABA swap test");
			ok &= expect(write_file(hook.replacement_path, replacement_model),
			             "write B before lock_existing ABA swap test");
			const howdy::native::user_model_store_test_hooks::ScopedHooks hooks({
			    .before_lock = [&hook](const std::filesystem::path &path) -> void {
				    install_replacement_before_lock(&hook, path);
			    },
			    .after_lock_before_revalidate = [&hook](const std::filesystem::path &path) -> void {
				    restore_original_after_lock(&hook, path);
			    },
			});
			const auto result = howdy::native::UserModelStore::lock_existing("alice");
			ok &= expect(hook.before_lock_calls == 1 && hook.after_lock_calls == 1,
			             "lock_existing ABA hooks execute exactly once");
			ok &= expect(hook.installed_b && hook.restored_a,
			             "lock_existing ABA test installs B before lock and restores A after lock");
			ok &= expect(result.status == howdy::native::UserModelStatus::kModelChanged,
			             "lock_existing rejects ABA inode replacement");
			ok &= expect(!result.transaction.has_value(),
			             "lock_existing ABA rejection creates no transaction");
			ok &= expect(read_file(model_path) == original_model,
			             "lock_existing ABA restored original remains unmodified");
			ok &= expect(read_file(hook.locked_replacement_saved_path) == replacement_model,
			             "lock_existing ABA locked replacement remains unmodified");
			fs::remove(model_path, ec);
			ec.clear();
			fs::remove(hook.locked_replacement_saved_path, ec);
			ec.clear();
		}
		ok &= expect(write_file(model_path, "not-json"),
		             "write malformed existing file before mutations");
		const auto malformed_before_mutation = read_file(model_path);
		{
			const auto result = howdy::native::append_user_model_entry("alice", first_entry);
			ok &= expect(result.status == howdy::native::UserModelStatus::kParseError,
			             "append rejects malformed existing file");
			ok &= expect(read_file(model_path) == malformed_before_mutation,
			             "failed append leaves malformed existing file unchanged");
		}
		{
			const auto result = howdy::native::remove_user_model_entry("alice", 0);
			ok &= expect(result.status == howdy::native::UserModelStatus::kParseError,
			             "remove rejects malformed existing file");
			ok &= expect(read_file(model_path) == malformed_before_mutation,
			             "failed remove leaves malformed existing file unchanged");
		}
		ok &= expect(write_file(model_path, R"({"id":1})"),
		             "write invalid-shape existing file before mutations");
		const auto invalid_shape_before_mutation = read_file(model_path);
		{
			const auto result = howdy::native::append_user_model_entry("alice", first_entry);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "append rejects invalid-shape existing file");
			ok &= expect(read_file(model_path) == invalid_shape_before_mutation,
			             "failed append leaves invalid-shape existing file unchanged");
		}
		{
			const auto result = howdy::native::remove_user_model_entry("alice", 0);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "remove rejects invalid-shape existing file");
			ok &= expect(read_file(model_path) == invalid_shape_before_mutation,
			             "failed remove leaves invalid-shape existing file unchanged");
		}
		const auto deeply_nested_model =
		    R"([{"id":0,"time":1,"label":"deep","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.1]],"unknown":)" +
		    nested_array(howdy::native::user_model_limits::kMaxJsonNestingDepth) + "}]";
		ok &= expect(write_file(model_path, deeply_nested_model),
		             "write deeply nested unknown field before mutations");
		const auto deeply_nested_before_mutation = read_file(model_path);
		{
			const auto result = howdy::native::append_user_model_entry("alice", first_entry);
			ok &= expect(result.status == howdy::native::UserModelStatus::kOversized,
			             "append rejects deeply nested unknown field");
			ok &= expect(read_file(model_path) == deeply_nested_before_mutation,
			             "deep nesting append failure leaves model file unchanged");
		}
		{
			const auto result = howdy::native::remove_user_model_entry("alice", 0);
			ok &= expect(result.status == howdy::native::UserModelStatus::kOversized,
			             "remove rejects deeply nested unknown field");
			ok &= expect(read_file(model_path) == deeply_nested_before_mutation,
			             "deep nesting remove failure leaves model file unchanged");
		}
		fs::remove(model_path, ec);
		ec.clear();
		const auto first_append = howdy::native::append_user_model_entry("alice", first_entry);
		ok &= expect(first_append.status == howdy::native::UserModelStatus::kOk,
		             "append creates first model entry");
		ok &= expect(first_append.entry.id == 0, "append allocates first model ID");
		{
			const auto                             before_oversized_append = read_file(model_path);
			const howdy::native::NewUserModelEntry oversized_entry{
			    .label = std::string(static_cast<std::size_t>(
			                             howdy::native::user_model_limits::kMaxUserModelFileBytes),
			                         'x'),
			    .backend   = backend,
			    .metric    = "cosine",
			    .model     = "sface.onnx",
			    .encodings = {{0.3F, 0.4F}},
			};
			const auto result = howdy::native::append_user_model_entry("alice", oversized_entry);
			ok &= expect(result.status == howdy::native::UserModelStatus::kWriteFailed,
			             "append rejects serialized model larger than read limit");
			ok &= expect(read_file(model_path) == before_oversized_append,
			             "oversized append leaves existing model file unchanged");
		}
		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":0,"time":1,"label":"first","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.1,0.2]],"future_field":"preserved"}])"),
		    "add unknown field to existing model entry");

		const howdy::native::NewUserModelEntry second_entry{
		    .label     = "second",
		    .backend   = backend,
		    .metric    = "cosine",
		    .model     = "sface.onnx",
		    .encodings = {{0.3F, 0.4F}},
		};
		const howdy::native::NewUserModelEntry invalid_encoding_entry{
		    .label     = "invalid",
		    .backend   = backend,
		    .metric    = "cosine",
		    .model     = "sface.onnx",
		    .encodings = {{std::numeric_limits<float>::infinity()}},
		};
		{
			const auto result =
			    howdy::native::append_user_model_entry("alice", invalid_encoding_entry);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "append rejects invalid new-entry encodings before writing");
			std::ifstream     persisted(model_path);
			const std::string persisted_text((std::istreambuf_iterator<char>(persisted)),
			                                 std::istreambuf_iterator<char>());
			ok &= expect(!persisted_text.contains("invalid"),
			             "append does not write invalid new-entry encodings");
		}
		const auto second_append = howdy::native::append_user_model_entry("alice", second_entry);
		ok &= expect(second_append.status == howdy::native::UserModelStatus::kOk,
		             "append adds second model entry");
		ok &= expect(second_append.entry.id == 1, "append allocates next model ID");
		{
			const auto result =
			    howdy::native::list_user_model_entries("alice", backend, "cosine", "sface.onnx");
			ok &= expect(result.status == howdy::native::UserModelStatus::kOk,
			             "lifecycle listing loads appended entries");
			ok &= expect(result.entries.size() == 2, "append preserves existing entries");
			ok &= expect(result.entries[0].label == "first" && result.entries[1].label == "second",
			             "lifecycle listing preserves entry order");
			ok &= expect(result.next_id == 2, "lifecycle listing reports next model ID");
			std::ifstream     persisted(model_path);
			const std::string persisted_text((std::istreambuf_iterator<char>(persisted)),
			                                 std::istreambuf_iterator<char>());
			ok &= expect(persisted_text.contains("future_field"),
			             "append preserves unknown fields in existing entries");
		}
		return ok;
	}
}  // namespace howdy::test::user_models
