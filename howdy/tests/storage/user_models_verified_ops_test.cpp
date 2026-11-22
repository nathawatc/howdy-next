#include "storage/user_model_status.hpp"
#include "storage/user_model_store.hpp"
#include "storage/user_model_types.hpp"
#include "storage/user_models_test_support.hpp"
#include "support/bit_flags.hpp"
#include "support/face_metric.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

#include <unistd.h>

#include <sys/stat.h>

namespace howdy::test::user_models {

	auto TestUserModelVerifiedOperations() -> bool {
		namespace fs                 = std::filesystem;
		bool              ok         = true;
		const auto        temp_root  = fs::current_path() / "howdy-user-models-verified-ops-test";
		const auto        models_dir = temp_root / "models";
		const auto        model_path = models_dir / "alice.dat";
		const std::string backend    = "opencv_dnn_sface";
		std::error_code   ec;
		fs::remove_all(temp_root, ec);
		ec.clear();
		fs::create_directories(models_dir, ec);
		ok &= Expect(!ec, "create verified-ops temp models directory");
		ec.clear();
		const howdy::test::TemporaryDirectory temp_guard(temp_root);
		const auto                            store = MakeTestStore(models_dir, temp_root);

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
		ok &= Expect(store.AppendEntry("alice", first_entry).status ==
		                 howdy::native::UserModelStatus::kOk,
		             "append initial verified-ops entry");
		ok &= Expect(store.AppendEntry("alice", second_entry).status ==
		                 howdy::native::UserModelStatus::kOk,
		             "append second verified-ops entry");

		{
			const howdy::native::UserModelEntryExpectation missing_expected{
			    .id      = 99,
			    .time    = 1,
			    .label   = "missing",
			    .backend = backend,
			    .metric  = howdy::native::FaceMetric::kCosine,
			    .model   = "sface.onnx",
			};
			const auto result = store.RemoveEntryIfMatches("alice", missing_expected);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kModelChanged,
			             "verified remove reports model changed when expected entry is missing");
		}
		{
			const auto listing = store.ListEntries("alice", backend, "sface.onnx");
			ok &= Expect(listing.status == howdy::native::UserModelStatus::kOk &&
			                 listing.entries.size() == 2,
			             "list entries before verified stale remove");
			const auto expected = ExpectationFromEntry(listing.entries.at(0));
			ok &= Expect(
			    WriteFile(
			        model_path,
			        R"([{"id":0,"time":2,"label":"changed","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.1,0.2]],"future_field":"preserved"},{"id":1,"time":1,"label":"second","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.3,0.4]]}])"),
			    "rewrite model entry after remove listing");
			const auto result = store.RemoveEntryIfMatches("alice", expected);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kModelChanged,
			             "verified remove aborts when model entry changes after listing");
			const auto after = store.ListEntries("alice", backend, "sface.onnx");
			ok &= Expect(after.status == howdy::native::UserModelStatus::kOk &&
			                 after.entries.size() == 2 && after.entries.at(0).label == "changed",
			             "stale verified remove leaves changed model entry");
		}
		ok &= Expect(
		    WriteFile(
		        model_path,
		        R"([{"id":0,"time":1,"label":"first","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.1,0.2]],"future_field":"preserved"},{"id":1,"time":1,"label":"second","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.3,0.4]]}])"),
		    "restore unchanged entries before verified remove");
		{
			const auto listing = store.ListEntries("alice", backend, "sface.onnx");
			ok &= Expect(listing.status == howdy::native::UserModelStatus::kOk &&
			                 listing.entries.size() == 2,
			             "list entries before verified remove");
			const auto expected = ExpectationFromEntry(listing.entries.at(0));
			const auto result   = store.RemoveEntryIfMatches("alice", expected);
			ok &=
			    Expect(result.status == howdy::native::UserModelStatus::kOk && !result.removed_last,
				       "verified remove succeeds when model entry is unchanged");
			ok &= Expect(result.entry.id == 0 && result.entry.label == "first",
			             "verified remove returns actual removed entry");
			const auto remaining = store.ListEntries("alice", backend);
			ok &= Expect(remaining.entries.size() == 1 && remaining.entries.at(0).id == 1,
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
			const auto result = store.RemoveEntryIfMatches("alice", expected_0);
			ok &=
			    Expect(result.status == howdy::native::UserModelStatus::kOk && !result.removed_last,
				       "remove deletes existing model ID");
			ok &= Expect(result.entry.id == 0 && result.entry.label == "first",
			             "remove returns actual removed entry");
			const auto remaining = store.ListEntries("alice", backend);
			ok &= Expect(remaining.entries.size() == 1 && remaining.entries.at(0).id == 1,
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
			const auto result = store.RemoveEntryIfMatches("alice", expected_1);
			ok &=
			    Expect(result.status == howdy::native::UserModelStatus::kOk && result.removed_last,
				       "remove deletes last model entry");
			ok &= Expect(!fs::exists(model_path), "remove last entry deletes model file");
		}

		ok &= Expect(store.AppendEntry("alice", first_entry).status ==
		                 howdy::native::UserModelStatus::kOk,
		             "append recreates model before clear");
		{
			const auto snapshot = RequireSnapshot(store, "alice");
			ok &= Expect(snapshot.has_value(), "inspect model file before clear");
			if (snapshot.has_value()) {
				const auto result = store.ClearEntriesIfUnchanged("alice", *snapshot);
				ok &= Expect(result.status == howdy::native::UserModelStatus::kOk,
				             "clear removes all model entries");
				ok &= Expect(!fs::exists(model_path), "clear deletes model file");
			}
		}

		const auto created_models_dir = temp_root / "created-store-models";
		fs::remove_all(created_models_dir, ec);
		ec.clear();
		const auto created_store = MakeTestStore(created_models_dir, temp_root);
		{
			const auto result = created_store.AppendEntry("created-user", first_entry);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kOk,
			             "append creates missing secure models directory");
			struct stat dir_stat{};
			struct stat file_stat{};
			const auto  created_model_path = created_models_dir / "created-user.dat";
			ok &= Expect(stat(created_models_dir.c_str(), &dir_stat) == 0,
			             "stat append-created models directory");
			ok &= Expect(stat(created_model_path.c_str(), &file_stat) == 0,
			             "stat append-created model file");
			ok &= Expect(howdy::native::MaskedBits(dir_stat.st_mode, 0777) == 0750,
			             "append-created models directory uses 0750 mode");
			ok &= Expect(howdy::native::MaskedBits(file_stat.st_mode, 0777) == 0600,
			             "append-created model file uses 0600 mode");
		}
		{
			const howdy::native::NewUserModelEntry empty_encoding_entry{
			    .label   = "rejected",
			    .backend = backend,
			    .metric  = howdy::native::FaceMetric::kCosine,
			    .model   = "sface.onnx",
			};
			const auto result = created_store.AppendEntry("rejected-user", empty_encoding_entry);
			const auto rejected_path = created_models_dir / "rejected-user.dat";
			ok &= Expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "invalid first append is rejected");
			ok &= Expect(!fs::exists(rejected_path),
			             "invalid first append leaves nothing at the model path");
		}
		fs::remove_all(created_models_dir, ec);
		ec.clear();

		const auto stale_lock_path = fs::path(model_path.string() + ".lock");
		fs::remove(stale_lock_path, ec);
		ec.clear();
		WhenSupported(
		    symlink("/tmp", stale_lock_path.c_str()) == 0,
		    [&] -> void {
			    const auto result = store.AppendEntry("alice", first_entry);
			    ok &= Expect(result.status == howdy::native::UserModelStatus::kOk,
				             "stale sidecar lock symlink does not affect descriptor locking");
			    ok &= Expect(fs::remove(stale_lock_path, ec), "remove stale lock symlink");
			    ec.clear();
		    },
		    "stale lock symlink creation failed");

		return ok;
	}

}  // namespace howdy::test::user_models
