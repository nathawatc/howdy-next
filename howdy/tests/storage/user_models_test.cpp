#include "storage/user_models_test_support.hpp"
#include "support/user_names.hpp"

#include <thread>

namespace howdy::test::user_models {
	using namespace howdy::test::user_models;

	auto test_user_model_loading() -> bool {
		namespace fs = std::filesystem;

		bool            ok        = true;
		const auto      temp_root = fs::current_path() / "howdy-user-models-test";
		std::error_code ec;
		unlink((temp_root / "models" / "alice.dat").c_str());
		fs::remove_all(temp_root, ec);
		fs::create_directories(temp_root, ec);
		ok &= expect(!ec, "create temp root");

		const auto models_dir = temp_root / "models";
		setenv("HOWDY_USER_MODELS_DIR", models_dir.c_str(), 1);
		ok &= expect_readiness_checks(temp_root);
		{
			const auto result = howdy::native::list_user_model_entries("alice", {});
			ok &= expect(result.status == howdy::native::UserModelStatus::kNoModelDirectory,
			             "missing model directory returns kNoModelDirectory");
		}
		{
			const auto result = howdy::native::load_user_models("alice", "opencv_dnn_sface");
			ok &= expect(result.status == howdy::native::UserModelStatus::kNoModel,
			             "load remaps missing model directory to kNoModel");
		}
		{
			const auto result = howdy::native::inspect_user_model_file("alice");
			ok &= expect(result.status == howdy::native::UserModelStatus::kNoModelDirectory,
			             "inspect returns kNoModelDirectory");
		}
		fs::create_directories(models_dir, ec);
		ok &= expect(!ec, "create models dir");

		const std::string backend = "opencv_dnn_sface";
		ok &= expect(howdy::native::is_valid_model_user_name("alice@example.com"),
		             "domain-style usernames remain valid");
		ok &= expect(!howdy::native::is_valid_model_user_name("../alice"),
		             "malformed path-traversal username input is rejected");
		ok &= expect(!howdy::native::is_valid_model_user_name("alice..bob"),
		             "malformed username containing dot-dot is rejected");
		ok &= expect(!howdy::native::resolve_user_model_path(models_dir, "../alice").has_value(),
		             "unsafe model paths are not constructed");
		ok &= expect(!howdy::native::resolve_user_model_path(models_dir, "alice..bob").has_value(),
		             "dot-dot username input is rejected before model path construction");

		{
			const auto result = howdy::native::load_user_models("../alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInvalidUser,
			             "invalid username returns kInvalidUser");
		}

		{
			const auto result = howdy::native::load_user_models("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kNoModel,
			             "missing file returns kNoModel");
		}
		{
			const auto result = howdy::native::inspect_user_model_file("alice");
			ok &= expect(result.status == howdy::native::UserModelStatus::kNoModel,
			             "inspect returns kNoModel");
		}

		const auto model_path = models_dir / "alice.dat";
		when_supported(
		    mkfifo(model_path.c_str(), 0600) == 0,
		    [&] -> void {
			    const auto started = std::chrono::steady_clock::now();
			    const auto result  = howdy::native::load_user_models("alice", backend);
			    const auto elapsed = std::chrono::steady_clock::now() - started;
			    ok &= expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
			                 "FIFO model target is rejected");
			    ok &= expect(elapsed < std::chrono::seconds(1),
			                 "FIFO model target is rejected without blocking");
			    ok &= expect(fs::remove(model_path, ec), "remove FIFO model target");
			    ec.clear();
		    },
		    "FIFO model target creation failed");
		ok &= expect(write_file(model_path, "not-json"), "write malformed model file");
		{
			const auto result = howdy::native::inspect_user_model_file("alice");
			ok &= expect(result.status == howdy::native::UserModelStatus::kOk,
			             "inspect accepts malformed JSON without parsing");
		}
		{
			const auto result = howdy::native::load_user_models("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kParseError,
			             "malformed json returns kParseError");
		}

		ok &= expect(write_file(model_path, "[]"), "write empty model list");
		{
			const auto result = howdy::native::load_user_models("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kNoModel,
			             "empty model list returns kNoModel");
		}

		ok &= expect(
		    write_file(model_path,
		               R"([{"id":1,"label":"bad","backend":"other_backend","data":[[0.1,0.2]]}])"),
		    "write incompatible backend model");
		{
			const auto result = howdy::native::load_user_models("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kIncompatibleBackend,
			             "incompatible backend is detected");
		}

		ok &= expect(write_file(model_path,
		                        R"([
{"id":7,"label":"first","backend":"opencv_dnn_sface","data":[[0.1,0.2],[0.3,0.4]]},
{"id":8,"label":"second","backend":"opencv_dnn_sface","data":["ignored",[1.1,1.2,1.3]]}
])"),
		             "write valid models");
		{
			const auto result = howdy::native::load_user_models("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kOk,
			             "valid models return kOk");
			ok &= expect(result.stored.encodings.size() == 3, "three valid encodings loaded");
			ok &=
			    expect(result.stored.models.size() == 3, "model metadata count matches encodings");
			ok &=
			    expect(result.stored.models[0].id == 7 && result.stored.models[0].label == "first",
			           "first model metadata preserved");
			ok &=
			    expect(result.stored.models[2].id == 8 && result.stored.models[2].label == "second",
			           "second model metadata preserved");
		}

		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":9,"label":"bad/name","backend":"opencv_dnn_sface","data":[[0.1,0.2]]}])"),
		    "write unsafe label model");
		{
			const auto result = howdy::native::load_user_models("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kParseError,
			             "model label containing path separator is rejected");
		}

		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":9,"label":"bad\nname","backend":"opencv_dnn_sface","data":[[0.1,0.2]]}])"),
		    "write control-character label model");
		{
			const auto result = howdy::native::load_user_models("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kParseError,
			             "model label containing newline is rejected");
		}

		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":7,"label":"first","backend":"opencv_dnn_sface","data":[[0.1,0.2]]}])"),
		    "restore valid model after unsafe label");

		const auto symlink_target = models_dir / "target.dat";
		ok &= expect(
		    write_file(
		        symlink_target,
		        R"([{"id":10,"label":"target","backend":"opencv_dnn_sface","data":[[0.1,0.2]]}])"),
		    "write symlink target model");
		fs::remove(model_path, ec);
		ec.clear();
		when_supported(
		    symlink(symlink_target.c_str(), model_path.c_str()) == 0,
		    [&] -> void {
			    const auto result = howdy::native::load_user_models("alice", backend);
			    ok &= expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
			                 "symlinked model file is rejected");
			    ok &= expect(fs::remove(model_path, ec), "remove symlinked model path");
			    ec.clear();
		    },
		    "model symlink creation failed");
		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":7,"label":"first","backend":"opencv_dnn_sface","data":[[0.1,0.2]]}])"),
		    "restore valid model after symlink");

		fs::remove(model_path, ec);
		ec.clear();
		when_supported(
		    symlink(model_path.c_str(), model_path.c_str()) == 0,
		    [&] -> void {
			    const auto result = howdy::native::list_user_model_entries("alice", backend);
			    ok &= expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
			                 "self-referential model symlink is rejected");
			    ok &= expect(unlink(model_path.c_str()) == 0,
			                 "remove self-referential model symlink");
			    ec.clear();
		    },
		    "model symlink loop creation failed");
		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":7,"label":"first","backend":"opencv_dnn_sface","data":[[0.1,0.2]]}])"),
		    "restore valid model after symlink loop");

		ok &= expect(chmod(model_path.c_str(), 0664) == 0, "make model file group-writable");
		{
			const auto result = howdy::native::load_user_models("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
			             "group-writable model file is rejected");
		}
		ok &= expect(chmod(model_path.c_str(), 0644) == 0, "restore model file mode");

		ok &= expect(chmod(model_path.c_str(), 0666) == 0, "make model file world-writable");
		{
			const auto result = howdy::native::load_user_models("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
			             "world-writable model file is rejected");
		}
		ok &= expect(chmod(model_path.c_str(), 0644) == 0, "restore model file mode");

		const auto hardlink_path = models_dir / "alice-hardlink.dat";
		ok &= expect(fs::remove(hardlink_path, ec) || !ec, "remove stale hardlink path");
		ec.clear();
		ok &= expect(link(model_path.c_str(), hardlink_path.c_str()) == 0,
		             "create hard-linked model file");
		{
			const auto result = howdy::native::load_user_models("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
			             "hard-linked model file is rejected");
		}
		ok &= expect(fs::remove(hardlink_path, ec), "remove hard-linked model file");
		ec.clear();

		when_supported(
		    geteuid() != 0,
		    [&] -> void {
			    ok &= expect(chmod(model_path.c_str(), 0000) == 0, "make model file unreadable");
			    const auto result = howdy::native::load_user_models("alice", backend);
			    ok &= expect(result.status != howdy::native::UserModelStatus::kOk,
			                 "unreadable model file fails safely");
			    ok &= expect(chmod(model_path.c_str(), 0644) == 0, "restore unreadable model file");
		    },
		    "unreadable model file check while running as root");

		ok &= expect(chmod(models_dir.c_str(), 0775) == 0, "make models dir group-writable");
		{
			const auto result = howdy::native::load_user_models("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
			             "group-writable model dir is rejected");
		}
		ok &= expect(chmod(models_dir.c_str(), 0755) == 0, "restore models dir mode");

		ok &= expect(chmod(models_dir.c_str(), 0777) == 0, "make models dir world-writable");
		{
			const auto result = howdy::native::load_user_models("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
			             "world-writable model dir is rejected");
		}
		ok &= expect(chmod(models_dir.c_str(), 0755) == 0, "restore models dir mode");

		std::string oversized_encoding =
		    R"([{"id":9,"label":"oversized","backend":"opencv_dnn_sface","data":[[)";
		for (int index = 0; index < 1100; ++index) {
			oversized_encoding += index > 0 ? ",0.1" : "0.1";
		}
		oversized_encoding += "]]}]";
		ok &= expect(write_file(model_path, oversized_encoding), "write oversized encoding");
		{
			const auto result = howdy::native::load_user_models("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kParseError,
			             "oversized encoding is rejected");
		}

		ok &= expect(write_file(model_path, R"({"id":1})"), "write wrong top-level JSON shape");
		{
			const auto result = howdy::native::list_user_model_entries("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "lifecycle listing rejects wrong top-level JSON shape");
		}
		{
			const auto result = howdy::native::load_user_models("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kParseError,
			             "load remaps invalid-shape model JSON to kParseError");
		}

		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":-1,"time":1,"label":"bad","backend":"opencv_dnn_sface","data":[[0.1]]}])"),
		    "write negative ID model");
		{
			const auto result = howdy::native::list_user_model_entries("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "lifecycle listing rejects negative model IDs");
		}

		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":1,"time":1,"label":"first","backend":"opencv_dnn_sface","data":[[0.1]]},{"id":1,"time":2,"label":"second","backend":"opencv_dnn_sface","data":[[0.2]]}])"),
		    "write duplicate ID models");
		{
			const auto result = howdy::native::list_user_model_entries("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "lifecycle listing rejects duplicate model IDs");
		}

		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":"1","time":1,"label":"bad","backend":"opencv_dnn_sface","data":[[0.1]]}])"),
		    "write non-integer ID model");
		{
			const auto result = howdy::native::list_user_model_entries("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "lifecycle listing rejects non-integer model IDs");
		}

		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":2147483647,"time":1,"label":"bad","backend":"opencv_dnn_sface","data":[[0.1]]}])"),
		    "write max-int ID model");
		{
			const auto result = howdy::native::list_user_model_entries("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "lifecycle listing rejects max-int model IDs");
		}

		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":1,"time":"now","label":"bad","backend":"opencv_dnn_sface","data":[[0.1]]}])"),
		    "write non-integer timestamp model");
		{
			const auto result = howdy::native::list_user_model_entries("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "lifecycle listing rejects non-integer timestamps");
		}

		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":1,"time":1,"label":7,"backend":"opencv_dnn_sface","data":[[0.1]]}])"),
		    "write non-string label model");
		{
			const auto result = howdy::native::list_user_model_entries("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "lifecycle listing rejects non-string labels");
		}

		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":1,"time":1,"label":"bad","backend":"opencv_dnn_sface","data":[[0.1,"bad"]]}])"),
		    "write non-numeric encoding value model");
		{
			const auto result = howdy::native::list_user_model_entries("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "lifecycle listing rejects non-numeric encoding values");
		}

		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":1,"time":1,"label":"bad","backend":"opencv_dnn_sface","data":[[0.1,[0.2]]]}])"),
		    "write nested encoding array model");
		{
			const auto result = howdy::native::list_user_model_entries("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "lifecycle listing rejects nested encoding arrays");
		}

		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":1,"time":1,"label":"bad","backend":"opencv_dnn_sface","data":[[0.1,{}]]}])"),
		    "write nested encoding object model");
		{
			const auto result = howdy::native::list_user_model_entries("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kInvalidShape,
			             "lifecycle listing rejects nested encoding objects");
		}

		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":1,"time":1,"label":"bad","backend":"opencv_dnn_sface","data":[[1e999]]}])"),
		    "write non-finite encoding value model");
		{
			const auto result = howdy::native::list_user_model_entries("alice", backend);
			ok &= expect(result.status != howdy::native::UserModelStatus::kOk,
			             "lifecycle listing rejects non-finite encoding values when representable");
		}

		ok &= expect(
		    write_file(
		        model_path,
		        R"([{"id":1,"label":"bad","backend":"opencv_dnn_sface","metric":"l2","model":"sface.onnx","data":[[0.1]]}])"),
		    "write incompatible metric model");
		{
			const auto result = howdy::native::list_user_model_entries(
			    "alice", backend, howdy::native::FaceMetric::kCosine, "sface.onnx");
			ok &= expect(result.status == howdy::native::UserModelStatus::kIncompatibleMetric,
			             "lifecycle listing rejects incompatible metric");
		}
		{
			const auto result = howdy::native::list_user_model_entries(
			    "alice", backend, howdy::native::FaceMetric::kL2, "other.onnx");
			ok &= expect(result.status == howdy::native::UserModelStatus::kIncompatibleModel,
			             "lifecycle listing rejects incompatible model metadata");
		}

		std::string oversized_json =
		    R"json([{"id":1,"label":"large","data":[[0.1]],"padding":")json";
		oversized_json.append((1024 * 1024) + 1, 'x');
		oversized_json += "\"}]";
		ok &= expect(write_file(model_path, oversized_json), "write oversized model JSON");
		{
			const auto result = howdy::native::list_user_model_entries("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kOversized,
			             "lifecycle listing rejects oversized model JSON");
		}
		{
			const auto result = howdy::native::load_user_models("alice", backend);
			ok &= expect(result.status == howdy::native::UserModelStatus::kParseError,
			             "load remaps oversized model JSON to kParseError");
		}

		fs::remove(model_path, ec);
		ec.clear();
		{
			const auto result = howdy::native::clear_user_model_entries("alice");
			ok &= expect(result.status == howdy::native::UserModelStatus::kNoModel,
			             "clear reports no model file without parsing");
		}

		ok &= expect(write_file(model_path, "not-json"), "write malformed model before clear");
		{
			const auto result = howdy::native::clear_user_model_entries("alice");
			ok &= expect(result.status == howdy::native::UserModelStatus::kOk,
			             "clear removes malformed JSON");
			ok &= expect(!fs::exists(model_path), "clear deletes malformed JSON model file");
		}

		ok &= expect(write_file(model_path, oversized_json), "write oversized model before clear");
		{
			const auto result = howdy::native::clear_user_model_entries("alice");
			ok &= expect(result.status == howdy::native::UserModelStatus::kOk,
			             "clear removes oversized JSON");
			ok &= expect(!fs::exists(model_path), "clear deletes oversized JSON model file");
		}

		ok &= expect(write_file(model_path, R"({"id":1})"), "write wrong-shape model before clear");
		{
			const auto result = howdy::native::clear_user_model_entries("alice");
			ok &= expect(result.status == howdy::native::UserModelStatus::kOk,
			             "clear removes wrong-shape JSON");
			ok &= expect(!fs::exists(model_path), "clear deletes wrong-shape JSON model file");
		}

		ok &= expect(write_file(model_path, "not-json"),
		             "write malformed model before verified clear");
		{
			const auto inspection = howdy::native::inspect_user_model_file("alice");
			ok &= expect(inspection.status == howdy::native::UserModelStatus::kOk &&
			                 inspection.snapshot.has_value(),
			             "verified clear inspects malformed JSON without parsing");
			with_present(inspection.snapshot, [&] -> void {
				const auto result = howdy::native::clear_user_model_entries_if_unchanged(
				    "alice", *inspection.snapshot);
				ok &= expect(result.status == howdy::native::UserModelStatus::kOk,
				             "verified clear removes unchanged malformed JSON");
			});
			ok &=
			    expect(!fs::exists(model_path), "verified clear deletes malformed JSON model file");
		}

		ok &= expect(write_file(model_path, oversized_json),
		             "write oversized model before verified clear");
		{
			const auto inspection = howdy::native::inspect_user_model_file("alice");
			ok &= expect(inspection.status == howdy::native::UserModelStatus::kOk &&
			                 inspection.snapshot.has_value(),
			             "verified clear inspects oversized JSON without parsing");
			with_present(inspection.snapshot, [&] -> void {
				const auto result = howdy::native::clear_user_model_entries_if_unchanged(
				    "alice", *inspection.snapshot);
				ok &= expect(result.status == howdy::native::UserModelStatus::kOk,
				             "verified clear removes unchanged oversized JSON");
			});
			ok &=
			    expect(!fs::exists(model_path), "verified clear deletes oversized JSON model file");
		}

		ok &= expect(write_file(model_path, R"({"id":1})"),
		             "write wrong-shape model before verified clear");
		{
			const auto inspection = howdy::native::inspect_user_model_file("alice");
			ok &= expect(inspection.status == howdy::native::UserModelStatus::kOk &&
			                 inspection.snapshot.has_value(),
			             "verified clear inspects wrong-shape JSON without parsing");
			with_present(inspection.snapshot, [&] -> void {
				const auto result = howdy::native::clear_user_model_entries_if_unchanged(
				    "alice", *inspection.snapshot);
				ok &= expect(result.status == howdy::native::UserModelStatus::kOk,
				             "verified clear removes unchanged wrong-shape JSON");
			});
			ok &= expect(!fs::exists(model_path),
			             "verified clear deletes wrong-shape JSON model file");
		}

		ok &= expect(write_file(model_path, "not-json"), "write model before stale verified clear");
		{
			const auto inspection = howdy::native::inspect_user_model_file("alice");
			ok &= expect(inspection.status == howdy::native::UserModelStatus::kOk &&
			                 inspection.snapshot.has_value(),
			             "verified clear captures file snapshot");
			ok &= expect(write_file(model_path, "changed-json"),
			             "rewrite model after clear inspection");
			with_present(inspection.snapshot, [&] -> void {
				const auto result = howdy::native::clear_user_model_entries_if_unchanged(
				    "alice", *inspection.snapshot);
				ok &= expect(result.status == howdy::native::UserModelStatus::kModelChanged,
				             "verified clear aborts when model file changes after inspection");
			});
			ok &= expect(fs::exists(model_path), "stale verified clear leaves changed model file");
		}

		ok &= expect(write_file(model_path, R"([{"id":0,"time":1,"label":"one","data":[[1.0]]}])"),
		             "write same-size model before stale clear");
		{
			const auto inspection = howdy::native::inspect_user_model_file("alice");
			ok &= expect(inspection.status == howdy::native::UserModelStatus::kOk &&
			                 inspection.snapshot.has_value(),
			             "verified clear captures snapshot before same-size rewrite");
			std::this_thread::sleep_for(std::chrono::milliseconds(1100));
			ok &= expect(
			    write_file(model_path, R"([{"id":0,"time":1,"label":"two","data":[[1.0]]}])"),
			    "rewrite model with same-size content after clear inspection");
			with_present(inspection.snapshot, [&] -> void {
				const std::array<timespec, 2> times{
				    timespec{.tv_sec  = inspection.snapshot->mtime_seconds,
				             .tv_nsec = inspection.snapshot->mtime_nanosecs},
				    timespec{.tv_sec  = inspection.snapshot->mtime_seconds,
				             .tv_nsec = inspection.snapshot->mtime_nanosecs},
				};
				ok &= expect(utimensat(AT_FDCWD, model_path.c_str(), times.data(), 0) == 0,
				             "restore old model mtime after same-size rewrite");
				const auto result = howdy::native::clear_user_model_entries_if_unchanged(
				    "alice", *inspection.snapshot);
				ok &= expect(result.status == howdy::native::UserModelStatus::kModelChanged,
				             "verified clear detects same-size rewrite with restored mtime");
			});
			ok &=
			    expect(fs::exists(model_path), "stale same-size verified clear leaves model file");
		}

		ok &= expect(
		    write_file(
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
			const auto result =
			    howdy::native::append_user_model_entry("alice", default_label_entry);
			ok &= expect(result.status == howdy::native::UserModelStatus::kOk,
			             "append assigns actual ID from locked storage state");
			ok &= expect(result.entry.id == 11, "append returns actual created model ID");
			ok &= expect(result.entry.label == "Model #11",
			             "append default label matches actual created model ID");
			const auto listing = howdy::native::list_user_model_entries(
			    "alice", backend, howdy::native::FaceMetric::kCosine, "sface.onnx");
			ok &= expect(listing.status == howdy::native::UserModelStatus::kOk &&
			                 listing.entries.size() == 2 && listing.entries[1].id == 11 &&
			                 listing.entries[1].label == "Model #11",
			             "stored default label matches actual created model ID");
		}
		return ok;
	}
}  // namespace howdy::test::user_models

auto main() -> int {
	const std::array results = {
	    howdy::test::user_models::test_user_model_loading(),
	    howdy::test::user_models::test_user_model_mutation_start(),
	    howdy::test::user_models::test_user_model_mutation_failures(),
	};
	const bool ok = std::ranges::all_of(results, [](bool value) -> bool {
		return value;
	});
	return ok ? 0 : 1;
}
