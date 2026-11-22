#include "storage/user_model_status.hpp"
#include "storage/user_model_store.hpp"
#include "storage/user_model_types.hpp"
#include "storage/user_models_test_support.hpp"
#include "support/face_metric.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>

namespace howdy::test::user_models {

	auto TestUserModelLoading() -> bool {
		namespace fs = std::filesystem;

		bool            ok        = true;
		const auto      temp_root = fs::current_path() / "howdy-user-models-test";
		std::error_code ec;
		unlink((temp_root / "models" / "alice.dat").c_str());
		fs::remove_all(temp_root, ec);
		fs::create_directories(temp_root, ec);
		ok &= Expect(!ec, "create temp root");

		const auto models_dir = temp_root / "models";
		const auto store      = MakeTestStore(models_dir, temp_root);
		ok &= ExpectReadinessChecks(temp_root);
		{
			const auto result = store.ListEntries("alice", {});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kNoModelDirectory,
			             "missing model directory returns kNoModelDirectory");
		}
		{
			const auto result = store.LoadModels("alice", "opencv_dnn_sface");
			ok &= Expect(result.status == howdy::native::UserModelStatus::kNoModel,
			             "load remaps missing model directory to kNoModel");
		}
		{
			const auto result = store.InspectFile("alice");
			ok &= Expect(result.status == howdy::native::UserModelStatus::kNoModelDirectory,
			             "inspect returns kNoModelDirectory");
		}
		fs::create_directories(models_dir, ec);
		ok &= Expect(!ec, "create models dir");

		const std::string backend = "opencv_dnn_sface";
		{
			const auto result = store.LoadModels("../alice", backend);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kInvalidUser,
			             "invalid username returns kInvalidUser");
		}

		{
			const auto result = store.LoadModels("alice", backend);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kNoModel,
			             "missing file returns kNoModel");
		}
		{
			const auto result = store.InspectFile("alice");
			ok &= Expect(result.status == howdy::native::UserModelStatus::kNoModel,
			             "inspect returns kNoModel");
		}

		const auto model_path = models_dir / "alice.dat";
		WhenSupported(
		    mkfifo(model_path.c_str(), 0600) == 0,
		    [&] -> void {
			    const auto started = std::chrono::steady_clock::now();
			    const auto result  = store.LoadModels("alice", backend);
			    const auto elapsed = std::chrono::steady_clock::now() - started;
			    ok &= Expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
				             "FIFO model target is rejected");
			    ok &= Expect(elapsed < std::chrono::seconds(1),
				             "FIFO model target is rejected without blocking");
			    ok &= Expect(fs::remove(model_path, ec), "remove FIFO model target");
			    ec.clear();
		    },
		    "FIFO model target creation failed");
		ok &= Expect(WriteFile(model_path, "not-json"), "write malformed model file");
		{
			const auto result = store.InspectFile("alice");
			ok &= Expect(result.status == howdy::native::UserModelStatus::kOk,
			             "inspect accepts malformed JSON without parsing");
		}
		{
			const auto result = store.LoadModels("alice", backend);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kParseError,
			             "malformed json returns kParseError");
		}

		ok &= Expect(WriteFile(model_path, "[]"), "write empty model list");
		{
			const auto result = store.LoadModels("alice", backend);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kNoModel,
			             "empty model list returns kNoModel");
		}

		ok &= Expect(
		    WriteFile(model_path,
			          R"([{"id":1,"label":"bad","backend":"other_backend","data":[[0.1,0.2]]}])"),
		    "write incompatible backend model");
		{
			const auto result = store.LoadModels("alice", backend);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kIncompatibleBackend,
			             "incompatible backend is detected");
		}

		ok &= Expect(WriteFile(model_path,
		                       R"([
{"id":7,"label":"first","backend":"opencv_dnn_sface","data":[[0.1,0.2],[0.3,0.4]]},
{"id":8,"label":"second","backend":"opencv_dnn_sface","data":["ignored",[1.1,1.2,1.3]]}
])"),
		             "write valid models");
		{
			const auto result = store.LoadModels("alice", backend);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kOk,
			             "valid models return kOk");
			ok &= Expect(result.stored.encodings.size() == 3, "three valid encodings loaded");
			ok &=
			    Expect(result.stored.models.size() == 3, "model metadata count matches encodings");
			ok &= Expect(result.stored.models.at(0).id == 7 &&
			                 result.stored.models.at(0).label == "first",
			             "first model metadata preserved");
			ok &= Expect(result.stored.models.at(2).id == 8 &&
			                 result.stored.models.at(2).label == "second",
			             "second model metadata preserved");
		}
		{
			const auto wrong_owner_store =
			    MakeTestStore(models_dir, temp_root, static_cast<uid_t>(geteuid() + 1));
			ok &= Expect(store.LoadModels("alice", backend).status ==
			                 howdy::native::UserModelStatus::kOk,
			             "explicit matching owner accepts model");
			ok &= Expect(wrong_owner_store.LoadModels("alice", backend).status ==
			                 howdy::native::UserModelStatus::kInsecurePath,
			             "explicit mismatched owner rejects load");
			ok &= Expect(wrong_owner_store.ListEntries("alice", backend).status ==
			                 howdy::native::UserModelStatus::kInsecurePath,
			             "explicit mismatched owner rejects list");
			ok &= Expect(wrong_owner_store.InspectFile("alice").status ==
			                 howdy::native::UserModelStatus::kInsecurePath,
			             "explicit mismatched owner rejects inspect");
		}

		ok &= Expect(
		    WriteFile(
		        model_path,
		        R"([{"id":9,"label":"bad/name","backend":"opencv_dnn_sface","data":[[0.1,0.2]]}])"),
		    "write unsafe label model");
		{
			const auto result = store.LoadModels("alice", backend);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kParseError,
			             "model label containing path separator is rejected");
		}

		ok &= Expect(
		    WriteFile(
		        model_path,
		        R"([{"id":9,"label":"bad\nname","backend":"opencv_dnn_sface","data":[[0.1,0.2]]}])"),
		    "write control-character label model");
		{
			const auto result = store.LoadModels("alice", backend);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kParseError,
			             "model label containing newline is rejected");
		}

		ok &= Expect(
		    WriteFile(
		        model_path,
		        R"([{"id":7,"label":"first","backend":"opencv_dnn_sface","data":[[0.1,0.2]]}])"),
		    "restore valid model after unsafe label");

		const auto symlink_target = models_dir / "target.dat";
		ok &= Expect(
		    WriteFile(
		        symlink_target,
		        R"([{"id":10,"label":"target","backend":"opencv_dnn_sface","data":[[0.1,0.2]]}])"),
		    "write symlink target model");
		fs::remove(model_path, ec);
		ec.clear();
		WhenSupported(
		    symlink(symlink_target.c_str(), model_path.c_str()) == 0,
		    [&] -> void {
			    const auto result = store.LoadModels("alice", backend);
			    ok &= Expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
				             "symlinked model file is rejected");
			    ok &= Expect(fs::remove(model_path, ec), "remove symlinked model path");
			    ec.clear();
		    },
		    "model symlink creation failed");
		ok &= Expect(
		    WriteFile(
		        model_path,
		        R"([{"id":7,"label":"first","backend":"opencv_dnn_sface","data":[[0.1,0.2]]}])"),
		    "restore valid model after symlink");

		fs::remove(model_path, ec);
		ec.clear();
		WhenSupported(
		    symlink(model_path.c_str(), model_path.c_str()) == 0,
		    [&] -> void {
			    const auto result = store.ListEntries("alice", backend);
			    ok &= Expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
				             "self-referential model symlink is rejected");
			    ok &= Expect(unlink(model_path.c_str()) == 0,
				             "remove self-referential model symlink");
			    ec.clear();
		    },
		    "model symlink loop creation failed");
		ok &= Expect(
		    WriteFile(
		        model_path,
		        R"([{"id":7,"label":"first","backend":"opencv_dnn_sface","data":[[0.1,0.2]]}])"),
		    "restore valid model after symlink loop");

		ok &= Expect(chmod(model_path.c_str(), 0664) == 0, "make model file group-writable");
		{
			const auto result = store.LoadModels("alice", backend);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
			             "group-writable model file is rejected");
		}
		ok &= Expect(chmod(model_path.c_str(), 0644) == 0, "restore model file mode");

		ok &= Expect(chmod(model_path.c_str(), 0666) == 0, "make model file world-writable");
		{
			const auto result = store.LoadModels("alice", backend);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
			             "world-writable model file is rejected");
		}
		ok &= Expect(chmod(model_path.c_str(), 0644) == 0, "restore model file mode");

		const auto hardlink_path = models_dir / "alice-hardlink.dat";
		ok &= Expect(fs::remove(hardlink_path, ec) || !ec, "remove stale hardlink path");
		ec.clear();
		ok &= Expect(link(model_path.c_str(), hardlink_path.c_str()) == 0,
		             "create hard-linked model file");
		{
			const auto result = store.LoadModels("alice", backend);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
			             "hard-linked model file is rejected");
		}
		ok &= Expect(fs::remove(hardlink_path, ec), "remove hard-linked model file");
		ec.clear();

		WhenSupported(
		    geteuid() != 0,
		    [&] -> void {
			    ok &= Expect(chmod(model_path.c_str(), 0000) == 0, "make model file unreadable");
			    const auto result = store.LoadModels("alice", backend);
			    ok &= Expect(result.status != howdy::native::UserModelStatus::kOk,
				             "unreadable model file fails safely");
			    ok &= Expect(chmod(model_path.c_str(), 0644) == 0, "restore unreadable model file");
		    },
		    "unreadable model file check while running as root");

		ok &= Expect(chmod(models_dir.c_str(), 0775) == 0, "make models dir group-writable");
		{
			const auto result = store.LoadModels("alice", backend);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
			             "group-writable model dir is rejected");
		}
		ok &= Expect(chmod(models_dir.c_str(), 0755) == 0, "restore models dir mode");

		ok &= Expect(chmod(models_dir.c_str(), 0777) == 0, "make models dir world-writable");
		{
			const auto result = store.LoadModels("alice", backend);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
			             "world-writable model dir is rejected");
		}
		ok &= Expect(chmod(models_dir.c_str(), 0755) == 0, "restore models dir mode");

		std::string oversized_encoding =
		    R"([{"id":9,"label":"oversized","backend":"opencv_dnn_sface","data":[[)";
		for (int index = 0; index < 1100; ++index) {
			oversized_encoding += index > 0 ? ",0.1" : "0.1";
		}
		oversized_encoding += "]]}]";
		ok &= Expect(WriteFile(model_path, oversized_encoding), "write oversized encoding");
		{
			const auto result = store.LoadModels("alice", backend);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kParseError,
			             "oversized encoding is rejected");
		}

		ok &= Expect(WriteFile(model_path, R"({"id":1})"), "write wrong top-level JSON shape");
		{
			const auto result = store.ListEntries("alice", backend);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "lifecycle listing rejects wrong top-level JSON shape");
		}
		{
			const auto result = store.LoadModels("alice", backend);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kParseError,
			             "load remaps invalid-shape model JSON to kParseError");
		}

		ok &= Expect(
		    WriteFile(
		        model_path,
		        R"([{"id":-1,"time":1,"label":"bad","backend":"opencv_dnn_sface","data":[[0.1]]}])"),
		    "write negative ID model");
		{
			const auto result = store.ListEntries("alice", backend);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "lifecycle listing rejects negative model IDs");
		}

		ok &= Expect(
		    WriteFile(
		        model_path,
		        R"([{"id":1,"time":1,"label":"first","backend":"opencv_dnn_sface","data":[[0.1]]},{"id":1,"time":2,"label":"second","backend":"opencv_dnn_sface","data":[[0.2]]}])"),
		    "write duplicate ID models");
		{
			const auto result = store.ListEntries("alice", backend);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "lifecycle listing rejects duplicate model IDs");
		}

		ok &= Expect(
		    WriteFile(
		        model_path,
		        R"([{"id":"1","time":1,"label":"bad","backend":"opencv_dnn_sface","data":[[0.1]]}])"),
		    "write non-integer ID model");
		{
			const auto result = store.ListEntries("alice", backend);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "lifecycle listing rejects non-integer model IDs");
		}

		ok &= Expect(
		    WriteFile(
		        model_path,
		        R"([{"id":2147483647,"time":1,"label":"bad","backend":"opencv_dnn_sface","data":[[0.1]]}])"),
		    "write max-int ID model");
		{
			const auto result = store.ListEntries("alice", backend);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "lifecycle listing rejects max-int model IDs");
		}

		ok &= Expect(
		    WriteFile(
		        model_path,
		        R"([{"id":1,"time":"now","label":"bad","backend":"opencv_dnn_sface","data":[[0.1]]}])"),
		    "write non-integer timestamp model");
		{
			const auto result = store.ListEntries("alice", backend);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "lifecycle listing rejects non-integer timestamps");
		}

		ok &= Expect(
		    WriteFile(
		        model_path,
		        R"([{"id":1,"time":1,"label":7,"backend":"opencv_dnn_sface","data":[[0.1]]}])"),
		    "write non-string label model");
		{
			const auto result = store.ListEntries("alice", backend);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "lifecycle listing rejects non-string labels");
		}

		ok &= Expect(
		    WriteFile(
		        model_path,
		        R"([{"id":1,"time":1,"label":"bad","backend":"opencv_dnn_sface","data":[[0.1,"bad"]]}])"),
		    "write non-numeric encoding value model");
		{
			const auto result = store.ListEntries("alice", backend);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "lifecycle listing rejects non-numeric encoding values");
		}

		ok &= Expect(
		    WriteFile(
		        model_path,
		        R"([{"id":1,"time":1,"label":"bad","backend":"opencv_dnn_sface","data":[[0.1,[0.2]]]}])"),
		    "write nested encoding array model");
		{
			const auto result = store.ListEntries("alice", backend);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "lifecycle listing rejects nested encoding arrays");
		}

		ok &= Expect(
		    WriteFile(
		        model_path,
		        R"([{"id":1,"time":1,"label":"bad","backend":"opencv_dnn_sface","data":[[0.1,{}]]}])"),
		    "write nested encoding object model");
		{
			const auto result = store.ListEntries("alice", backend);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "lifecycle listing rejects nested encoding objects");
		}

		ok &= Expect(
		    WriteFile(
		        model_path,
		        R"([{"id":1,"time":1,"label":"bad","backend":"opencv_dnn_sface","data":[[1e999]]}])"),
		    "write non-finite encoding value model");
		{
			const auto result = store.ListEntries("alice", backend);
			ok &= Expect(result.status != howdy::native::UserModelStatus::kOk,
			             "lifecycle listing rejects non-finite encoding values when representable");
		}

		ok &= Expect(
		    WriteFile(
		        model_path,
		        R"([{"id":1,"label":"bad","backend":"opencv_dnn_sface","metric":"l2","model":"sface.onnx","data":[[0.1]]}])"),
		    "write model stored under a different metric");
		{
			const auto result = store.ListEntries("alice", backend, "other.onnx");
			ok &= Expect(result.status == howdy::native::UserModelStatus::kIncompatibleModel,
			             "lifecycle listing rejects incompatible model metadata");
		}

		std::string oversized_json =
		    R"json([{"id":1,"label":"large","data":[[0.1]],"padding":")json";
		oversized_json.append((1024 * 1024) + 1, 'x');
		oversized_json += "\"}]";
		ok &= Expect(WriteFile(model_path, oversized_json), "write oversized model JSON");
		{
			const auto result = store.ListEntries("alice", backend);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kOversized,
			             "lifecycle listing rejects oversized model JSON");
		}
		{
			const auto result = store.LoadModels("alice", backend);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kParseError,
			             "load remaps oversized model JSON to kParseError");
		}

		fs::remove(model_path, ec);
		ec.clear();
		{
			const howdy::native::UserModelFileSnapshot stale_snapshot{
			    .dev            = 1,
			    .inode          = 2,
			    .size           = 3,
			    .mtime_seconds  = 4,
			    .mtime_nanosecs = 5,
			    .ctime_seconds  = 6,
			    .ctime_nanosecs = 7,
			};
			const auto result = store.ClearEntriesIfUnchanged("alice", stale_snapshot);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kModelChanged,
			             "verified clear reports model changed when model file is missing");
		}

		ok &= Expect(WriteFile(model_path, "not-json"),
		             "write malformed model before verified clear");
		{
			const auto inspection = store.InspectFile("alice");
			ok &= Expect(inspection.status == howdy::native::UserModelStatus::kOk &&
			                 inspection.snapshot.has_value(),
			             "verified clear inspects malformed JSON without parsing");
			WithPresent(inspection.snapshot, [&] -> void {
				const auto result = store.ClearEntriesIfUnchanged("alice", *inspection.snapshot);
				ok &= Expect(result.status == howdy::native::UserModelStatus::kOk,
				             "verified clear removes unchanged malformed JSON");
			});
			ok &=
			    Expect(!fs::exists(model_path), "verified clear deletes malformed JSON model file");
		}

		ok &= Expect(WriteFile(model_path, oversized_json),
		             "write oversized model before verified clear");
		{
			const auto inspection = store.InspectFile("alice");
			ok &= Expect(inspection.status == howdy::native::UserModelStatus::kOk &&
			                 inspection.snapshot.has_value(),
			             "verified clear inspects oversized JSON without parsing");
			WithPresent(inspection.snapshot, [&] -> void {
				const auto result = store.ClearEntriesIfUnchanged("alice", *inspection.snapshot);
				ok &= Expect(result.status == howdy::native::UserModelStatus::kOk,
				             "verified clear removes unchanged oversized JSON");
			});
			ok &=
			    Expect(!fs::exists(model_path), "verified clear deletes oversized JSON model file");
		}

		ok &= Expect(WriteFile(model_path, R"({"id":1})"),
		             "write wrong-shape model before verified clear");
		{
			const auto inspection = store.InspectFile("alice");
			ok &= Expect(inspection.status == howdy::native::UserModelStatus::kOk &&
			                 inspection.snapshot.has_value(),
			             "verified clear inspects wrong-shape JSON without parsing");
			WithPresent(inspection.snapshot, [&] -> void {
				const auto result = store.ClearEntriesIfUnchanged("alice", *inspection.snapshot);
				ok &= Expect(result.status == howdy::native::UserModelStatus::kOk,
				             "verified clear removes unchanged wrong-shape JSON");
			});
			ok &= Expect(!fs::exists(model_path),
			             "verified clear deletes wrong-shape JSON model file");
		}

		ok &= Expect(WriteFile(model_path, "not-json"), "write model before stale verified clear");
		{
			const auto inspection = store.InspectFile("alice");
			ok &= Expect(inspection.status == howdy::native::UserModelStatus::kOk &&
			                 inspection.snapshot.has_value(),
			             "verified clear captures file snapshot");
			ok &= Expect(WriteFile(model_path, "changed-json"),
			             "rewrite model after clear inspection");
			WithPresent(inspection.snapshot, [&] -> void {
				const auto result = store.ClearEntriesIfUnchanged("alice", *inspection.snapshot);
				ok &= Expect(result.status == howdy::native::UserModelStatus::kModelChanged,
				             "verified clear aborts when model file changes after inspection");
			});
			ok &= Expect(fs::exists(model_path), "stale verified clear leaves changed model file");
		}

		ok &= Expect(WriteFile(model_path, R"([{"id":0,"time":1,"label":"one","data":[[1.0]]}])"),
		             "write same-size model before stale clear");
		{
			const auto inspection = store.InspectFile("alice");
			ok &= Expect(inspection.status == howdy::native::UserModelStatus::kOk &&
			                 inspection.snapshot.has_value(),
			             "verified clear captures snapshot before same-size rewrite");
			std::this_thread::sleep_for(std::chrono::milliseconds(1100));
			ok &=
			    Expect(WriteFile(model_path, R"([{"id":0,"time":1,"label":"two","data":[[1.0]]}])"),
				       "rewrite model with same-size content after clear inspection");
			WithPresent(inspection.snapshot, [&] -> void {
				const std::array<timespec, 2> times{
				    timespec{
				        .tv_sec  = inspection.snapshot->mtime_seconds,
				        .tv_nsec = inspection.snapshot->mtime_nanosecs,
				    },
				    timespec{
				        .tv_sec  = inspection.snapshot->mtime_seconds,
				        .tv_nsec = inspection.snapshot->mtime_nanosecs,
				    },
				};
				ok &= Expect(utimensat(AT_FDCWD, model_path.c_str(), times.data(), 0) == 0,
				             "restore old model mtime after same-size rewrite");
				const auto result = store.ClearEntriesIfUnchanged("alice", *inspection.snapshot);
				ok &= Expect(result.status == howdy::native::UserModelStatus::kModelChanged,
				             "verified clear detects same-size rewrite with restored mtime");
			});
			ok &=
			    Expect(fs::exists(model_path), "stale same-size verified clear leaves model file");
		}

		ok &= Expect(
		    WriteFile(
		        model_path,
		        R"([{"id":10,"time":1,"label":"existing","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[0.1,0.2]]}])"),
		    "write existing model before default-label append");
		{
			const howdy::native::NewUserModelEntry default_label_entry{
			    .label     = "",
			    .backend   = backend,
			    .metric    = howdy::native::FaceMetric::kCosine,
			    .model     = "sface.onnx",
			    .encodings = {{0.3F, 0.4F}},
			};
			const auto result = store.AppendEntry("alice", default_label_entry);
			ok &= Expect(result.status == howdy::native::UserModelStatus::kOk,
			             "append assigns actual ID from locked storage state");
			ok &= Expect(result.entry.id == 11, "append returns actual created model ID");
			ok &= Expect(result.entry.label == "Model #11",
			             "append default label matches actual created model ID");
			const auto listing = store.ListEntries("alice", backend, "sface.onnx");
			ok &= Expect(listing.status == howdy::native::UserModelStatus::kOk &&
			                 listing.entries.size() == 2 && listing.entries.at(1).id == 11 &&
			                 listing.entries.at(1).label == "Model #11",
			             "stored default label matches actual created model ID");
		}
		return ok;
	}

	namespace {

		// Production() reads HOWDY_USER_MODELS_DIR (honoured while euid == uid) and requires owner
		// UID 0; every other test builds an explicit store.
		auto TestProductionStore() -> bool {
			namespace fs = std::filesystem;
			using howdy::native::UserModelStatus;

			bool            ok = true;
			std::error_code ec;
			const auto      list_production =
			    [](const fs::path &models_dir) -> howdy::native::UserModelListResult {
				const howdy::test::ScopedEnvironmentVariable models_env("HOWDY_USER_MODELS_DIR",
				                                                        models_dir.native());
				return howdy::native::UserModelStore::Production().ListEntries("alice", {});
			};

			const auto missing = fs::path("/howdy-production-store-test-missing-models");
			ok &= Expect(!fs::exists(missing, ec), "production test models dir is absent");
			ok &= Expect(list_production(missing).status == UserModelStatus::kNoModelDirectory,
			             "Production() resolves the models directory from HOWDY_USER_MODELS_DIR");

			const auto temp_root  = fs::current_path() / "howdy-user-models-production-test";
			const auto models_dir = temp_root / "models";
			fs::remove_all(temp_root, ec);
			fs::create_directories(models_dir, ec);
			ok &=
			    Expect(!ec && chmod(models_dir.c_str(), 0750) == 0, "create production models dir");
			// A tree this process owns fails the UID 0 owner check unless the process is root.
			if (geteuid() != 0) {
				const auto result = list_production(models_dir);
				ok &= Expect(result.status == UserModelStatus::kInsecurePath &&
				                 result.error_message.contains("owned by UID 0"),
				             "Production() requires models owned by UID 0");
			}
			fs::remove_all(temp_root, ec);
			return ok;
		}

	}  // namespace

	auto TestUserModelStatusClassification() -> bool {
		using howdy::native::Classify;
		using howdy::native::UserModelOutcome;
		using howdy::native::UserModelStatus;

		struct ClassificationCase {
			UserModelStatus  status;
			UserModelOutcome expected;
		};

		constexpr auto cases = std::to_array<ClassificationCase>({
		    {.status = UserModelStatus::kOk, .expected = UserModelOutcome::kOk},
		    {.status = UserModelStatus::kNoModel, .expected = UserModelOutcome::kAbsent},
		    {.status = UserModelStatus::kNoModelDirectory, .expected = UserModelOutcome::kAbsent},
		    {
		        .status   = UserModelStatus::kIncompatibleBackend,
		        .expected = UserModelOutcome::kFailedUncommitted,
		    },
		    {
		        .status   = UserModelStatus::kIncompatibleModel,
		        .expected = UserModelOutcome::kFailedUncommitted,
		    },
		    {
		        .status   = UserModelStatus::kParseError,
		        .expected = UserModelOutcome::kFailedUncommitted,
		    },
		    {
		        .status   = UserModelStatus::kInvalidShape,
		        .expected = UserModelOutcome::kFailedUncommitted,
		    },
		    {
		        .status   = UserModelStatus::kOversized,
		        .expected = UserModelOutcome::kFailedUncommitted,
		    },
		    {.status = UserModelStatus::kInsecurePath, .expected = UserModelOutcome::kInsecure},
		    {
		        .status   = UserModelStatus::kInvalidUser,
		        .expected = UserModelOutcome::kFailedUncommitted,
		    },
		    {
		        .status   = UserModelStatus::kLockFailed,
		        .expected = UserModelOutcome::kFailedUncommitted,
		    },
		    {
		        .status   = UserModelStatus::kWriteFailed,
		        .expected = UserModelOutcome::kFailedUncommitted,
		    },
		    {
		        .status   = UserModelStatus::kAtomicExchangeUnsupported,
		        .expected = UserModelOutcome::kFailedUncommitted,
		    },
		    {
		        .status   = UserModelStatus::kDeleteFailed,
		        .expected = UserModelOutcome::kFailedUncommitted,
		    },
		    {
		        .status   = UserModelStatus::kDurabilityUncertain,
		        .expected = UserModelOutcome::kCommittedNotDurable,
		    },
		    {
		        .status   = UserModelStatus::kCommitStateUncertain,
		        .expected = UserModelOutcome::kUncertain,
		    },
		    {
		        .status   = UserModelStatus::kDirectoryCreateFailed,
		        .expected = UserModelOutcome::kFailedUncommitted,
		    },
		    {.status = UserModelStatus::kModelChanged, .expected = UserModelOutcome::kChanged},
		});

		static_assert(Classify(UserModelStatus::kOk) == UserModelOutcome::kOk);
		static_assert(Classify(UserModelStatus::kNoModel) == UserModelOutcome::kAbsent);
		static_assert(Classify(UserModelStatus::kNoModelDirectory) == UserModelOutcome::kAbsent);
		static_assert(Classify(UserModelStatus::kDurabilityUncertain) ==
		              UserModelOutcome::kCommittedNotDurable);
		static_assert(Classify(UserModelStatus::kCommitStateUncertain) ==
		              UserModelOutcome::kUncertain);

		bool ok = true;
		for (const auto &[status, expected] : cases) {
			ok &= Expect(Classify(status) == expected, "Classify maps every UserModelStatus");
		}
		// NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
		const auto unknown = static_cast<UserModelStatus>(255);
		ok &= Expect(Classify(unknown) == UserModelOutcome::kUncertain,
		             "Classify fails closed to kUncertain on out-of-range status");
		return ok;
	}
}  // namespace howdy::test::user_models

auto main() -> int {
	const std::array results = {
	    howdy::test::user_models::TestUserModelStatusClassification(),
	    howdy::test::user_models::TestProductionStore(),
	    howdy::test::user_models::TestUserModelLoading(),
	    howdy::test::user_models::TestUserModelDescriptorLoading(),
	    howdy::test::user_models::TestUserModelMutationStart(),
	    howdy::test::user_models::TestUserModelMutationFailures(),
	    howdy::test::user_models::TestUserModelVerifiedOperations(),
	};
	const bool ok = std::ranges::all_of(results, [](bool value) -> bool {
		return value;
	});
	return ok ? 0 : 1;
}
