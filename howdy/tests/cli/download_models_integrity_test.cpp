#include "cli/download_models/internal.hpp"
#include "cli/download_models_test_support.hpp"
#include "support/atomic_files.hpp"
#include "support/scoped_stream_capture.hpp"
#include "test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <system_error>

#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>

namespace howdy::test::download_models {

	using howdy::test::Expect;

	namespace {

		auto TestExistingModelSecurity(const std::filesystem::path &temp_root) -> bool {
			namespace fs = std::filesystem;
			using howdy::native::ScopedStreamCapture;
			using howdy::native::download_models_internal::DownloadModelsDependencies;
			using howdy::native::download_models_internal::ExistingModelAction;
			using howdy::native::download_models_internal::InspectExistingModel;

			bool            ok = true;
			std::error_code ec;
			const auto      dir = temp_root / "inspect-existing-security";
			fs::remove_all(dir, ec);
			fs::create_directories(dir, ec);
			ok &= Expect(!ec, "create inspect-existing-security dir");

			const DownloadModelsDependencies dependencies{
			    .download_file        = SuccessfulFakeDownloadFile,
			    .model_file_owner_uid = TestModelFileOwnerUid,
			};

			// 1. FIFO with no writer must open non-blocking and abort as insecure.
			const auto fifo_path = dir / "fifo.onnx";
			ok &= Expect(mkfifo(fifo_path.c_str(), 0644) == 0, "create existing-model FIFO");
			{
				ScopedStreamCapture capture(std::cout);
				const auto          action =
				    InspectExistingModel(fifo_path, kTestModel, std::nullopt, dependencies);
				const auto output = capture.Release();
				ok &=
				    Expect(action == ExistingModelAction::kAbort && output.contains("regular file"),
					       "InspectExistingModel aborts on FIFO without blocking");
			}

			// 2. End-to-end download-models also rejects a pre-existing FIFO without blocking.
			const auto fifo_models_dir = dir / "fifo-models";
			const auto fifo_dest       = fifo_models_dir / kTestModel.filename;
			const auto fifo_output     = dir / "fifo-output.txt";
			fs::create_directories(fifo_models_dir, ec);
			ok &= Expect(!ec && mkfifo(fifo_dest.c_str(), 0644) == 0,
			             "create pre-existing FIFO at model destination");
			const std::array test_models = {kTestModel};
			int              fifo_exit   = 0;
			ok &= Expect(RunTestDownload({.models_dir = fifo_models_dir, .output = fifo_output},
			                             &fifo_exit, test_models, SuccessfulFakeDownloadFile),
			             "run download-models with FIFO destination");
			ok &= Expect(fifo_exit == EXIT_FAILURE && AttemptedDownloads() == 0 &&
			                 ReadFile(fifo_output).contains("regular file"),
			             "download-models aborts on FIFO without blocking or downloading");

			// 3. Symlink, group-writable, hard-linked, and wrong-owner files abort in
			//    InspectExistingModel even if swapped after CheckOpencvModelReadiness().
			const auto regular_path = dir / "regular.onnx";
			ok &= Expect(WriteFile(regular_path, kTestModelContent), "write regular model");

			const auto symlink_path = dir / "symlink.onnx";
			fs::create_symlink(regular_path, symlink_path, ec);
			ok &= Expect(!ec, "create symlink model");
			{
				ScopedStreamCapture capture(std::cout);
				const auto          action =
				    InspectExistingModel(symlink_path, kTestModel, std::nullopt, dependencies);
				const auto output = capture.Release();
				ok &= Expect(action == ExistingModelAction::kAbort &&
				                 output.contains("Failed to open existing model"),
				             "InspectExistingModel aborts on symlink");
			}

			const auto writable_path = dir / "group-writable.onnx";
			ok &= Expect(WriteFile(writable_path, kTestModelContent) &&
			                 chmod(writable_path.c_str(), 0664) == 0,
			             "create group-writable model");
			{
				ScopedStreamCapture capture(std::cout);
				const auto          action =
				    InspectExistingModel(writable_path, kTestModel, std::nullopt, dependencies);
				const auto output = capture.Release();
				ok &= Expect(action == ExistingModelAction::kAbort && output.contains("writable"),
				             "InspectExistingModel aborts on group-writable file");
			}

			const auto hard_link_src = dir / "hard-link-src.onnx";
			const auto hard_link_dst = dir / "hard-link-dst.onnx";
			ok &= Expect(WriteFile(hard_link_src, kTestModelContent), "create hard-link source");
			fs::create_hard_link(hard_link_src, hard_link_dst, ec);
			ok &= Expect(!ec, "create hard-link model");
			{
				ScopedStreamCapture capture(std::cout);
				const auto          action =
				    InspectExistingModel(hard_link_dst, kTestModel, std::nullopt, dependencies);
				const auto output = capture.Release();
				ok &=
				    Expect(action == ExistingModelAction::kAbort && output.contains("hard-linked"),
					       "InspectExistingModel aborts on hard-linked file");
			}

			const uid_t wrong_owner = getuid() == 0 ? static_cast<uid_t>(1) : static_cast<uid_t>(0);
			{
				ScopedStreamCapture capture(std::cout);
				const auto          action =
				    InspectExistingModel(regular_path, kTestModel, wrong_owner, dependencies);
				const auto output = capture.Release();
				ok &= Expect(action == ExistingModelAction::kAbort &&
				                 output.contains("UID " + std::to_string(wrong_owner)),
				             "InspectExistingModel aborts on owner mismatch");
			}

			// 4. Missing required callbacks fail closed before any filesystem open().
			const auto missing_path = dir / "does-not-exist.onnx";
			for (const auto &bad_dependencies : std::to_array<DownloadModelsDependencies>({
			         {
			             .download_file        = SuccessfulFakeDownloadFile,
			             .model_file_owner_uid = TestModelFileOwnerUid,
			             .sha256_file          = nullptr,
			             .fstat_file           = ::fstat,
			         },
			         {
			             .download_file        = SuccessfulFakeDownloadFile,
			             .model_file_owner_uid = TestModelFileOwnerUid,
			             .sha256_file =
			                 howdy::native::download_models_internal::Sha256FileDescriptor,
			             .fstat_file = nullptr,
			         },
			     })) {
				ScopedStreamCapture capture(std::cout);
				const auto          action =
				    InspectExistingModel(missing_path, kTestModel, std::nullopt, bad_dependencies);
				const auto output = capture.Release();
				ok &= Expect(action == ExistingModelAction::kAbort && output.empty(),
				             "InspectExistingModel fails closed on null callback before open");
			}

			return ok;
		}

	}  // namespace

	auto RunDownloadModelsIntegrityTests() -> bool {
		namespace fs              = std::filesystem;
		bool            ok        = true;
		const auto      temp_root = fs::current_path() / "howdy-download-models-test";
		std::error_code ec;

		const std::array test_models         = {kTestModel};
		const auto       matching_models_dir = temp_root / "matching-model";
		const auto       matching_model      = matching_models_dir / kTestModel.filename;
		const auto       matching_output     = temp_root / "matching-output.txt";
		fs::create_directories(matching_models_dir, ec);
		ok &= Expect(!ec && WriteFile(matching_model, kTestModelContent),
		             "create matching installed test model");
		int matching_exit                      = 0;
		DependenciesState().download_succeeds  = true;
		DependenciesState().downloaded_content = kTestModelContent;
		ok &= Expect(RunTestDownload({.models_dir = matching_models_dir, .output = matching_output},
		                             &matching_exit, test_models, SuccessfulFakeDownloadFile),
		             "run matching installed model");
		ok &= Expect(matching_exit == 0 && AttemptedDownloads() == 0,
		             "matching installed model skips download");
		ok &= Expect(ReadFile(matching_output).contains("Model already exists"),
		             "matching installed model reports already exists");

		int existing_hash_failure_exit = 0;
		ok &= Expect(RunTestDownload({.models_dir = matching_models_dir, .output = matching_output},
		                             &existing_hash_failure_exit, test_models,
		                             SuccessfulFakeDownloadFile, FailingSha256File),
		             "run existing-model hash operation failure");
		const auto existing_hash_failure_stdout = ReadFile(matching_output);
		ok &= Expect(existing_hash_failure_exit == EXIT_FAILURE && AttemptedDownloads() == 0,
		             "existing-model hash failure aborts before download");
		ok &= Expect(ReadFile(matching_model) == kTestModelContent,
		             "existing-model hash failure preserves destination");
		ok &= Expect(existing_hash_failure_stdout.contains("Failed to calculate SHA-256") &&
		                 !existing_hash_failure_stdout.contains("Model already exists") &&
		                 !existing_hash_failure_stdout.contains("Replacing invalid model"),
		             "existing-model hash failure reports operation failure only");

		DependenciesState().failing_fstat_attempt = 1;
		int existing_fstat_failure_exit           = 0;
		ok &= Expect(RunTestDownload({.models_dir = matching_models_dir, .output = matching_output},
		                             &existing_fstat_failure_exit, test_models,
		                             SuccessfulFakeDownloadFile,
		                             howdy::native::download_models_internal::Sha256FileDescriptor,
		                             SelectivelyFailingFstat),
		             "run existing-model fstat failure");
		const auto existing_fstat_failure_stdout = ReadFile(matching_output);
		ok &= Expect(existing_fstat_failure_exit == EXIT_FAILURE && AttemptedDownloads() == 0,
		             "existing-model fstat failure aborts before download");
		ok &= Expect(ReadFile(matching_model) == kTestModelContent,
		             "existing-model fstat failure preserves destination");
		ok &= Expect(existing_fstat_failure_stdout.contains("fstat") &&
		                 existing_fstat_failure_stdout.contains(matching_model.string()) &&
		                 existing_fstat_failure_stdout.contains("Input/output error") &&
		                 !existing_fstat_failure_stdout.contains("Size mismatch") &&
		                 !existing_fstat_failure_stdout.contains("Model already exists") &&
		                 !existing_fstat_failure_stdout.contains("Replacing invalid model"),
		             "existing-model fstat failure reports syscall context only");

		const auto replacement_models_dir = temp_root / "replacement-model";
		const auto replacement_model      = replacement_models_dir / kTestModel.filename;
		const auto replacement_output     = temp_root / "replacement-output.txt";
		fs::create_directories(replacement_models_dir, ec);
		ok &= Expect(!ec && WriteFile(replacement_model, ""), "create empty installed model");
		int replacement_exit = 0;
		ok &= Expect(
		    RunTestDownload({.models_dir = replacement_models_dir, .output = replacement_output},
			                &replacement_exit, test_models, SuccessfulFakeDownloadFile),
		    "run empty replacement");
		const auto replacement_stdout = ReadFile(replacement_output);
		ok &= Expect(replacement_exit == 0 && AttemptedDownloads() == 1 &&
		                 ReadFile(replacement_model) == kTestModelContent,
		             "empty installed model is replaced");
		ok &= Expect(replacement_stdout.contains("Replacing invalid model download") &&
		                 !replacement_stdout.contains("Model already exists"),
		             "corrupted first model is never reported already existing");

		ok &= Expect(WriteFile(replacement_model, "arbitrary existing data"),
		             "create arbitrary installed model");
		int arbitrary_exit = 0;
		ok &= Expect(
		    RunTestDownload({.models_dir = replacement_models_dir, .output = replacement_output},
			                &arbitrary_exit, test_models, SuccessfulFakeDownloadFile),
		    "run arbitrary replacement");
		ok &= Expect(arbitrary_exit == 0 && AttemptedDownloads() == 1,
		             "arbitrary installed model triggers replacement");
		const auto arbitrary_stdout = ReadFile(replacement_output);
		ok &= Expect(arbitrary_stdout.contains("Size mismatch") &&
		                 arbitrary_stdout.contains("expected 16, actual 23") &&
		                 !arbitrary_stdout.contains("Input/output error"),
		             "existing size mismatch reports expected and actual size only");

		ok &= Expect(WriteFile(replacement_model, "small test modeL"),
		             "create same-size wrong-hash installed model");
		int wrong_hash_exit = 0;
		ok &= Expect(
		    RunTestDownload({.models_dir = replacement_models_dir, .output = replacement_output},
			                &wrong_hash_exit, test_models, SuccessfulFakeDownloadFile),
		    "run wrong-hash replacement");
		ok &= Expect(wrong_hash_exit == 0 && AttemptedDownloads() == 1,
		             "wrong-hash installed model triggers replacement");

		ok &= Expect(WriteFile(replacement_model, "old destination"),
		             "create replacement destination");
		DependenciesState().downloaded_content = "small test modeL";
		int staged_mismatch_exit               = 0;
		ok &= Expect(
		    RunTestDownload({.models_dir = replacement_models_dir, .output = replacement_output},
			                &staged_mismatch_exit, test_models, SuccessfulFakeDownloadFile),
		    "run staged checksum mismatch");
		ok &= Expect(staged_mismatch_exit == EXIT_FAILURE &&
		                 ReadFile(replacement_model) == "old destination",
		             "wrong staged hash preserves destination");
		ok &= Expect(ReadFile(replacement_output).contains("Checksum mismatch"),
		             "same-size wrong staged hash reports checksum mismatch");
		ok &= Expect(CountFilesWithPrefix(replacement_models_dir, ".howdy-download-") == 0,
		             "wrong staged hash removes temporary file");

		ok &= Expect(WriteFile(replacement_model, "old destination"),
		             "restore destination for staged size mismatch");
		DependenciesState().downloaded_content = "short";
		int staged_size_exit                   = 0;
		ok &= Expect(
		    RunTestDownload({.models_dir = replacement_models_dir, .output = replacement_output},
			                &staged_size_exit, test_models, SuccessfulFakeDownloadFile),
		    "run staged size mismatch");
		ok &= Expect(staged_size_exit == EXIT_FAILURE &&
		                 ReadFile(replacement_model) == "old destination",
		             "staged size mismatch preserves destination");
		const auto staged_size_stdout = ReadFile(replacement_output);
		ok &= Expect(staged_size_stdout.contains("Size mismatch") &&
		                 staged_size_stdout.contains("expected 16, actual 5") &&
		                 !staged_size_stdout.contains("Input/output error"),
		             "staged size mismatch reports expected and actual size only");

		ok &= Expect(WriteFile(replacement_model, "small test modeL"),
		             "restore same-size invalid destination for staged fstat failure");
		DependenciesState().downloaded_content    = kTestModelContent;
		DependenciesState().failing_fstat_attempt = 2;
		int staged_fstat_exit                     = 0;
		ok &= Expect(
		    RunTestDownload({.models_dir = replacement_models_dir, .output = replacement_output},
			                &staged_fstat_exit, test_models, SuccessfulFakeDownloadFile,
			                howdy::native::download_models_internal::Sha256FileDescriptor,
			                SelectivelyFailingFstat),
		    "run staged fstat failure");
		const auto staged_fstat_stdout = ReadFile(replacement_output);
		ok &= Expect(staged_fstat_exit == EXIT_FAILURE &&
		                 ReadFile(replacement_model) == "small test modeL",
		             "staged fstat failure preserves destination");
		ok &= Expect(staged_fstat_stdout.contains("fstat") &&
		                 staged_fstat_stdout.contains(replacement_models_dir.string()) &&
		                 staged_fstat_stdout.contains(".howdy-download-") &&
		                 staged_fstat_stdout.contains("Input/output error") &&
		                 !staged_fstat_stdout.contains("Size mismatch") &&
		                 !staged_fstat_stdout.contains("Checksum mismatch"),
		             "staged fstat failure reports staged path and syscall context only");
		ok &= Expect(CountFilesWithPrefix(replacement_models_dir, ".howdy-download-") == 0,
		             "staged fstat failure removes temporary file");

		ok &= Expect(WriteFile(replacement_model, "old destination"),
		             "restore destination for hash read failure");
		int hash_read_failure_exit             = 0;
		DependenciesState().downloaded_content = kTestModelContent;
		ok &= Expect(
		    RunTestDownload({.models_dir = replacement_models_dir, .output = replacement_output},
			                &hash_read_failure_exit, test_models, SuccessfulFakeDownloadFile,
			                FailingSha256File),
		    "run staged hash operation failure");
		ok &= Expect(hash_read_failure_exit == EXIT_FAILURE &&
		                 ReadFile(replacement_model) == "old destination",
		             "hash read failure preserves destination");
		ok &= Expect(ReadFile(replacement_output).contains("Failed to calculate SHA-256") &&
		                 !ReadFile(replacement_output).contains("Checksum mismatch"),
		             "hash read failure is distinct from digest mismatch");
		ok &= Expect(CountFilesWithPrefix(replacement_models_dir, ".howdy-download-") == 0,
		             "hash read failure removes temporary file");

		const auto insecure_models_dir = temp_root / "insecure-model";
		const auto insecure_model      = insecure_models_dir / kTestModel.filename;
		const auto insecure_output     = temp_root / "insecure-output.txt";
		fs::create_directories(insecure_models_dir, ec);
		ok &= Expect(!ec && WriteFile(insecure_model, kTestModelContent) &&
		                 chmod(insecure_model.c_str(), 0664) == 0,
		             "create insecure installed model");
		int insecure_exit = 0;
		ok &= Expect(RunTestDownload({.models_dir = insecure_models_dir, .output = insecure_output},
		                             &insecure_exit, test_models, SuccessfulFakeDownloadFile),
		             "run insecure installed model");
		ok &= Expect(insecure_exit == EXIT_FAILURE && AttemptedDownloads() == 0,
		             "insecure installed model aborts before download");

		DependenciesState().downloaded_content = kTestModelContent;
		int installed_exit                     = 0;
		ok &= Expect(
		    RunTestDownload({.models_dir = replacement_models_dir, .output = replacement_output},
			                &installed_exit, test_models, SuccessfulFakeDownloadFile),
		    "run successful matching replacement");
		ok &= Expect(installed_exit == 0 && ReadFile(replacement_model) == kTestModelContent,
		             "matching staged test model installs atomically");
		int second_run_exit = 0;
		ok &= Expect(
		    RunTestDownload({.models_dir = replacement_models_dir, .output = replacement_output},
			                &second_run_exit, test_models, SuccessfulFakeDownloadFile),
		    "run installed matching test model");
		ok &= Expect(second_run_exit == 0 && AttemptedDownloads() == 0,
		             "second run performs zero downloads");

		const auto overflow_destination = temp_root / "overflow-write.onnx";
		auto       overflow_staged =
		    howdy::native::PrepareStagedFile(overflow_destination, ".howdy-download-");
		ok &=
		    Expect(overflow_staged.has_value(), "prepare staged file for overflow write callback");
		if (overflow_staged.has_value()) {
			howdy::native::download_models_internal::DownloadWriteContext write_context{
			    .staged = &*overflow_staged,
			};
			std::array<char, 2> payload        = {'x', 'y'};
			const auto          overflow_size  = (std::numeric_limits<std::size_t>::max() / 2) + 2;
			const auto          overflow_count = static_cast<std::size_t>(2);
			const auto          result =
			    howdy::native::download_models_internal::DownloadModelsWriteCallback(
			        payload.data(), overflow_size, overflow_count, &write_context);
			ok &= Expect(result == 0, "overflowing write callback returns 0");
			ok &= Expect(ReadFile(overflow_staged->Path()).empty(),
			             "overflowing write callback leaves staged file empty");
			overflow_staged->Cleanup();
		}

		const auto limited_destination = temp_root / "limited-write.onnx";
		auto       limited_staged =
		    howdy::native::PrepareStagedFile(limited_destination, ".howdy-download-");
		ok &= Expect(limited_staged.has_value(), "prepare staged file for cumulative write limit");
		if (limited_staged.has_value()) {
			howdy::native::download_models_internal::DownloadWriteContext write_context{
			    .staged    = &*limited_staged,
			    .max_bytes = 3,
			};
			std::array<char, 2> first_payload  = {'x', 'y'};
			std::array<char, 2> second_payload = {'z', '!'};
			const auto          first_result =
			    howdy::native::download_models_internal::DownloadModelsWriteCallback(
			        first_payload.data(), 1, first_payload.size(), &write_context);
			const auto second_result =
			    howdy::native::download_models_internal::DownloadModelsWriteCallback(
			        second_payload.data(), 1, second_payload.size(), &write_context);
			ok &= Expect(first_result == first_payload.size(),
			             "write callback accepts data within cumulative limit");
			ok &= Expect(second_result == 0,
			             "write callback rejects data exceeding cumulative limit");
			ok &= Expect(write_context.bytes_written == first_payload.size(),
			             "write callback tracks only committed bytes");
			ok &= Expect(ReadFile(limited_staged->Path()) == "xy",
			             "write callback leaves rejected chunk unwritten");
			limited_staged->Cleanup();
		}

		ok &= TestExistingModelSecurity(temp_root);
		return ok;
	}

}  // namespace howdy::test::download_models
