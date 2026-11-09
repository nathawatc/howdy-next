#include "../../src/storage/user_model_readiness/internal.hpp"
#include "storage/user_model_codec.hpp"
#include "storage/user_model_readiness.hpp"
#include "storage/user_model_status.hpp"
#include "storage/user_model_types.hpp"
#include "storage/user_models.hpp"
#include "storage/user_models_test_support.hpp"
#include "support/face_metric.hpp"
#include "support/file_security.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/stat.h>

namespace howdy::test::user_models {

	auto TestUserModelLoading() -> bool {
		namespace fs = std::filesystem;

		bool            ok        = true;
		const auto      temp_root = fs::current_path() / "howdy-user-models-test";
		std::error_code ec;
		unlink((temp_root / "models" / "alice.model").c_str());
		fs::remove_all(temp_root, ec);
		fs::create_directories(temp_root, ec);
		ok &= Expect(!ec, "create temp root");

		const auto models_dir = temp_root / "models";
		setenv("HOWDY_USER_MODELS_DIR", models_dir.c_str(), 1);
		ok &= ExpectReadinessChecks(temp_root);
		{
			const auto result = howdy::native::ListUserModelEntries("alice", {}, {}, {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kNoModelDirectory,
			             "missing model directory returns kNoModelDirectory");
		}
		{
			const auto result = howdy::native::LoadUserModels(
			    "alice", "opencv_dnn_sface", howdy::native::DefaultSecureOwnerUid(), {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kNoModel,
			             "load remaps missing model directory to kNoModel");
		}
		{
			const auto result = howdy::native::InspectUserModelFile("alice", {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kNoModelDirectory,
			             "inspect returns kNoModelDirectory");
		}
		fs::create_directories(models_dir, ec);
		ok &= Expect(!ec, "create models dir");

		const std::string backend = "opencv_dnn_sface";
		{
			const auto result = howdy::native::LoadUserModels(
			    "../alice", backend, howdy::native::DefaultSecureOwnerUid(), {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kInvalidUser,
			             "invalid username returns kInvalidUser");
		}

		{
			const auto result = howdy::native::LoadUserModels(
			    "alice", backend, howdy::native::DefaultSecureOwnerUid(), {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kNoModel,
			             "missing file returns kNoModel");
		}
		{
			const auto result = howdy::native::InspectUserModelFile("alice", {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kNoModel,
			             "inspect returns kNoModel");
		}

		const auto model_path = models_dir / "alice.model";
		WhenSupported(
		    mkfifo(model_path.c_str(), 0600) == 0,
		    [&] -> void {
			    const auto started = std::chrono::steady_clock::now();
			    const auto result  = howdy::native::LoadUserModels(
			        "alice", backend, howdy::native::DefaultSecureOwnerUid(), {temp_root});
			    const auto elapsed = std::chrono::steady_clock::now() - started;
			    ok &= Expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
				             "FIFO model target is rejected");
			    ok &= Expect(elapsed < std::chrono::seconds(1),
				             "FIFO model target is rejected without blocking");
			    ok &= Expect(fs::remove(model_path, ec), "remove FIFO model target");
			    ec.clear();
		    },
		    "FIFO model target creation failed");
		ok &= Expect(WriteFile(model_path, "not-a-model"), "write malformed model file");
		{
			const auto result = howdy::native::InspectUserModelFile("alice", {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kOk,
			             "inspect accepts a malformed model without decoding it");
		}
		{
			const auto result = howdy::native::LoadUserModels(
			    "alice", backend, howdy::native::DefaultSecureOwnerUid(), {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kParseError,
			             "malformed model returns kParseError");
		}

		ok &= Expect(WriteFile(model_path, ""), "write empty model file");
		{
			const auto result = howdy::native::LoadUserModels(
			    "alice", backend, howdy::native::DefaultSecureOwnerUid(), {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kParseError,
			             "empty model file returns kParseError");
		}

		auto other_backend    = ModelEntry(1, "bad");
		other_backend.backend = "other_backend";
		ok &= Expect(WriteFile(model_path, EncodeModels(std::vector{other_backend})),
		             "write incompatible backend model");
		{
			const auto result = howdy::native::LoadUserModels(
			    "alice", backend, howdy::native::DefaultSecureOwnerUid(), {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kIncompatibleBackend,
			             "incompatible backend is detected");
		}

		const auto first_model = EncodeModels(std::vector{ModelEntry(7, "first")});
		ok &= Expect(WriteFile(model_path, EncodeModels(std::vector{
		                                       ModelEntry(7, "first", {{0.1F, 0.2F}, {0.3F, 0.4F}}),
		                                       ModelEntry(8, "second", {{1.1F, 1.2F, 1.3F}}),
		                                   })),
		             "write valid models");
		{
			const auto result = howdy::native::LoadUserModels(
			    "alice", backend, howdy::native::DefaultSecureOwnerUid(), {temp_root});
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

		ok &= Expect(WriteFile(model_path, CorruptModelFile()), "write corrupted model");
		{
			const auto result = howdy::native::LoadUserModels(
			    "alice", backend, howdy::native::DefaultSecureOwnerUid(), {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kParseError,
			             "model failing its checksum is rejected");
		}

		ok &= Expect(WriteFile(model_path, first_model), "restore valid model after corruption");

		const auto symlink_target = models_dir / "target.model";
		ok &= Expect(WriteFile(symlink_target, EncodeModels(std::vector{ModelEntry(10, "target")})),
		             "write symlink target model");
		fs::remove(model_path, ec);
		ec.clear();
		WhenSupported(
		    symlink(symlink_target.c_str(), model_path.c_str()) == 0,
		    [&] -> void {
			    const auto result = howdy::native::LoadUserModels(
			        "alice", backend, howdy::native::DefaultSecureOwnerUid(), {temp_root});
			    ok &= Expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
				             "symlinked model file is rejected");
			    ok &= Expect(fs::remove(model_path, ec), "remove symlinked model path");
			    ec.clear();
		    },
		    "model symlink creation failed");
		ok &= Expect(WriteFile(model_path, first_model), "restore valid model after symlink");

		fs::remove(model_path, ec);
		ec.clear();
		WhenSupported(
		    symlink(model_path.c_str(), model_path.c_str()) == 0,
		    [&] -> void {
			    const auto result =
			        howdy::native::ListUserModelEntries("alice", backend, {}, {temp_root});
			    ok &= Expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
				             "self-referential model symlink is rejected");
			    ok &= Expect(unlink(model_path.c_str()) == 0,
				             "remove self-referential model symlink");
			    ec.clear();
		    },
		    "model symlink loop creation failed");
		ok &= Expect(WriteFile(model_path, first_model), "restore valid model after symlink loop");

		ok &= Expect(chmod(model_path.c_str(), 0664) == 0, "make model file group-writable");
		{
			const auto result = howdy::native::LoadUserModels(
			    "alice", backend, howdy::native::DefaultSecureOwnerUid(), {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
			             "group-writable model file is rejected");
		}
		ok &= Expect(chmod(model_path.c_str(), 0644) == 0, "restore model file mode");

		ok &= Expect(chmod(model_path.c_str(), 0666) == 0, "make model file world-writable");
		{
			const auto result = howdy::native::LoadUserModels(
			    "alice", backend, howdy::native::DefaultSecureOwnerUid(), {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
			             "world-writable model file is rejected");
		}
		ok &= Expect(chmod(model_path.c_str(), 0644) == 0, "restore model file mode");

		const auto hardlink_path = models_dir / "alice-hardlink.model";
		ok &= Expect(fs::remove(hardlink_path, ec) || !ec, "remove stale hardlink path");
		ec.clear();
		ok &= Expect(link(model_path.c_str(), hardlink_path.c_str()) == 0,
		             "create hard-linked model file");
		{
			const auto result = howdy::native::LoadUserModels(
			    "alice", backend, howdy::native::DefaultSecureOwnerUid(), {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
			             "hard-linked model file is rejected");
		}
		ok &= Expect(fs::remove(hardlink_path, ec), "remove hard-linked model file");
		ec.clear();

		{
			const int model_fd = open(model_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
			ok &= Expect(model_fd >= 0, "open canonical model for staged-path rejection checks");
			if (model_fd >= 0) {
				const auto       uid             = std::to_string(getuid());
				const auto       wrong_uid       = std::to_string(getuid() == 0 ? 1 : 0);
				const std::array malformed_paths = {
				    fs::path("/tmp/howdy/pam-" + uid + "-gen000/models/alice.model"),
				    fs::path("/run/howdy/pam-" + wrong_uid + "-gen000/models/alice.model"),
				    fs::path("/run/howdy/pam-" + uid + "-gen002/models/alice.model"),
				    fs::path("/run/howdy/pam-0" + uid + "-gen000/models/alice.model"),
				    fs::path("/run/howdy/pam-" + uid + "-gen000/not-models/alice.model"),
				    fs::path("/run/howdy/pam-" + uid + "-gen000/models/../alice.model"),
				};
				using howdy::native::user_model_readiness_internal::ClassifyStagedPath;
				using howdy::native::user_model_readiness_internal::StagedPathKind;
				ok &= Expect(ClassifyStagedPath(fs::path("/run/howdy/pam-" + uid +
				                                         "-gen000/models/alice.model")) ==
				                 StagedPathKind::kStaged,
				             "well-formed staged path is classified as staged");
				for (const auto &path : malformed_paths) {
					if (path.string().starts_with("/run/howdy/")) {
						ok &= Expect(ClassifyStagedPath(path) == StagedPathKind::kMalformed,
						             "staged classifier flags malformed path: " + path.string());
					}
					ok &= Expect(!howdy::native::ValidateStagedUserModelFile(model_fd, path),
					             "staged validator rejects malformed path: " + path.string());
				}
				ok &= Expect(
				    ClassifyStagedPath(fs::path("pam-" + uid + "-gen000/models/alice.model")) ==
				        StagedPathKind::kMalformed,
				    "relative staged-looking path is classified as malformed");
				const auto relative_staged_models = fs::path("pam-" + uid + "-gen000") / "models";
				const auto relative_staged        = howdy::native::CheckUserModelReadiness(
				    relative_staged_models, "alice", std::nullopt, {temp_root});
				ok &=
				    Expect(relative_staged.status == howdy::native::UserModelStatus::kInsecurePath,
					       "relative staged-looking model path is rejected as malformed");
				ok &= Expect(close(model_fd) == 0, "close staged-path rejection model");
			}
		}

		WhenSupported(
		    geteuid() != 0,
		    [&] -> void {
			    ok &= Expect(chmod(model_path.c_str(), 0000) == 0, "make model file unreadable");
			    const auto result = howdy::native::LoadUserModels(
			        "alice", backend, howdy::native::DefaultSecureOwnerUid(), {temp_root});
			    ok &= Expect(result.status != howdy::native::UserModelStatus::kOk,
				             "unreadable model file fails safely");
			    ok &= Expect(chmod(model_path.c_str(), 0644) == 0, "restore unreadable model file");
		    },
		    "unreadable model file check while running as root");

		ok &= Expect(chmod(models_dir.c_str(), 0775) == 0, "make models dir group-writable");
		{
			const auto result = howdy::native::LoadUserModels(
			    "alice", backend, howdy::native::DefaultSecureOwnerUid(), {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
			             "group-writable model dir is rejected");
		}
		ok &= Expect(chmod(models_dir.c_str(), 0755) == 0, "restore models dir mode");

		ok &= Expect(chmod(models_dir.c_str(), 0777) == 0, "make models dir world-writable");
		{
			const auto result = howdy::native::LoadUserModels(
			    "alice", backend, howdy::native::DefaultSecureOwnerUid(), {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kInsecurePath,
			             "world-writable model dir is rejected");
		}
		ok &= Expect(chmod(models_dir.c_str(), 0755) == 0, "restore models dir mode");

		ok &= Expect(WriteFile(model_path, first_model.substr(0, first_model.size() - 1)),
		             "write truncated model");
		{
			const auto result =
			    howdy::native::ListUserModelEntries("alice", backend, {}, {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kParseError,
			             "lifecycle listing rejects a truncated model");
		}
		{
			const auto result = howdy::native::LoadUserModels(
			    "alice", backend, howdy::native::DefaultSecureOwnerUid(), {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kParseError,
			             "load rejects a truncated model");
		}

		auto l2_model   = ModelEntry(1, "bad");
		l2_model.metric = howdy::native::FaceMetric::kL2;
		ok &= Expect(WriteFile(model_path, EncodeModels(std::vector{l2_model})),
		             "write model stored under a different metric");
		{
			const auto result =
			    howdy::native::ListUserModelEntries("alice", backend, "other.onnx", {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kIncompatibleModel,
			             "lifecycle listing rejects incompatible model metadata");
		}

		const std::string oversized_model(howdy::native::user_model_codec::kMaxFileBytes + 1, 'x');
		ok &= Expect(WriteFile(model_path, oversized_model), "write oversized model");
		{
			const auto result =
			    howdy::native::ListUserModelEntries("alice", backend, {}, {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kOversized,
			             "lifecycle listing rejects oversized model");
		}
		{
			const auto result = howdy::native::LoadUserModels(
			    "alice", backend, howdy::native::DefaultSecureOwnerUid(), {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kParseError,
			             "load remaps oversized model to kParseError");
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
			const auto result = howdy::native::ClearUserModelEntriesIfUnchanged(
			    "alice", stale_snapshot, {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kModelChanged,
			             "verified clear reports model changed when model file is missing");
		}

		ok &= Expect(WriteFile(model_path, "not-a-model"),
		             "write malformed model before verified clear");
		{
			const auto inspection = howdy::native::InspectUserModelFile("alice", {temp_root});
			ok &= Expect(inspection.status == howdy::native::UserModelStatus::kOk &&
			                 inspection.snapshot.has_value(),
			             "verified clear inspects a malformed model without decoding it");
			WithPresent(inspection.snapshot, [&] -> void {
				const auto result = howdy::native::ClearUserModelEntriesIfUnchanged(
				    "alice", *inspection.snapshot, {temp_root});
				ok &= Expect(result.status == howdy::native::UserModelStatus::kOk,
				             "verified clear removes an unchanged malformed model");
			});
			ok &= Expect(!fs::exists(model_path), "verified clear deletes a malformed model file");
		}

		ok &= Expect(WriteFile(model_path, oversized_model),
		             "write oversized model before verified clear");
		{
			const auto inspection = howdy::native::InspectUserModelFile("alice", {temp_root});
			ok &= Expect(inspection.status == howdy::native::UserModelStatus::kOk &&
			                 inspection.snapshot.has_value(),
			             "verified clear inspects an oversized model without decoding it");
			WithPresent(inspection.snapshot, [&] -> void {
				const auto result = howdy::native::ClearUserModelEntriesIfUnchanged(
				    "alice", *inspection.snapshot, {temp_root});
				ok &= Expect(result.status == howdy::native::UserModelStatus::kOk,
				             "verified clear removes an unchanged oversized model");
			});
			ok &= Expect(!fs::exists(model_path), "verified clear deletes an oversized model file");
		}

		ok &= Expect(WriteFile(model_path, CorruptModelFile()),
		             "write corrupted model before verified clear");
		{
			const auto inspection = howdy::native::InspectUserModelFile("alice", {temp_root});
			ok &= Expect(inspection.status == howdy::native::UserModelStatus::kOk &&
			                 inspection.snapshot.has_value(),
			             "verified clear inspects a corrupted model without decoding it");
			WithPresent(inspection.snapshot, [&] -> void {
				const auto result = howdy::native::ClearUserModelEntriesIfUnchanged(
				    "alice", *inspection.snapshot, {temp_root});
				ok &= Expect(result.status == howdy::native::UserModelStatus::kOk,
				             "verified clear removes an unchanged corrupted model");
			});
			ok &= Expect(!fs::exists(model_path), "verified clear deletes a corrupted model file");
		}

		ok &=
		    Expect(WriteFile(model_path, "not-a-model"), "write model before stale verified clear");
		{
			const auto inspection = howdy::native::InspectUserModelFile("alice", {temp_root});
			ok &= Expect(inspection.status == howdy::native::UserModelStatus::kOk &&
			                 inspection.snapshot.has_value(),
			             "verified clear captures file snapshot");
			ok &= Expect(WriteFile(model_path, "changed-model"),
			             "rewrite model after clear inspection");
			WithPresent(inspection.snapshot, [&] -> void {
				const auto result = howdy::native::ClearUserModelEntriesIfUnchanged(
				    "alice", *inspection.snapshot, {temp_root});
				ok &= Expect(result.status == howdy::native::UserModelStatus::kModelChanged,
				             "verified clear aborts when model file changes after inspection");
			});
			ok &= Expect(fs::exists(model_path), "stale verified clear leaves changed model file");
		}

		ok &= Expect(WriteFile(model_path, EncodeModels(std::vector{ModelEntry(0, "one")})),
		             "write same-size model before stale clear");
		{
			const auto inspection = howdy::native::InspectUserModelFile("alice", {temp_root});
			ok &= Expect(inspection.status == howdy::native::UserModelStatus::kOk &&
			                 inspection.snapshot.has_value(),
			             "verified clear captures snapshot before same-size rewrite");
			std::this_thread::sleep_for(std::chrono::milliseconds(1100));
			ok &= Expect(WriteFile(model_path, EncodeModels(std::vector{ModelEntry(0, "two")})),
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
				const auto result = howdy::native::ClearUserModelEntriesIfUnchanged(
				    "alice", *inspection.snapshot, {temp_root});
				ok &= Expect(result.status == howdy::native::UserModelStatus::kModelChanged,
				             "verified clear detects same-size rewrite with restored mtime");
			});
			ok &=
			    Expect(fs::exists(model_path), "stale same-size verified clear leaves model file");
		}

		ok &= Expect(WriteFile(model_path, EncodeModels(std::vector{ModelEntry(10, "existing")})),
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
			    howdy::native::AppendUserModelEntry("alice", default_label_entry, {temp_root});
			ok &= Expect(result.status == howdy::native::UserModelStatus::kOk,
			             "append assigns actual ID from locked storage state");
			ok &= Expect(result.entry.id == 11, "append returns actual created model ID");
			ok &= Expect(result.entry.label == "Model #11",
			             "append default label matches actual created model ID");
			const auto listing =
			    howdy::native::ListUserModelEntries("alice", backend, "sface.onnx", {temp_root});
			ok &= Expect(listing.status == howdy::native::UserModelStatus::kOk &&
			                 listing.entries.size() == 2 && listing.entries.at(1).id == 11 &&
			                 listing.entries.at(1).label == "Model #11",
			             "stored default label matches actual created model ID");
		}
		return ok;
	}
}  // namespace howdy::test::user_models

auto main() -> int {
	const std::array results = {
	    howdy::test::user_models::TestUserModelLoading(),
	    howdy::test::user_models::TestUserModelMutationStart(),
	    howdy::test::user_models::TestUserModelMutationFailures(),
	};
	const bool ok = std::ranges::all_of(results, [](bool value) -> bool {
		return value;
	});
	return ok ? 0 : 1;
}
