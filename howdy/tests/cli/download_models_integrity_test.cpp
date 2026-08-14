#include "cli/download_models_test_support.hpp"
#include "test_support.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>

namespace howdy::test::download_models {

	using howdy::test::expect;

	auto run_download_models_integrity_tests() -> bool {
		namespace fs              = std::filesystem;
		bool            ok        = true;
		const auto      temp_root = fs::current_path() / "howdy-download-models-test";
		std::error_code ec;

		const std::array test_models         = {kTestModel};
		const auto       matching_models_dir = temp_root / "matching-model";
		const auto       matching_model      = matching_models_dir / kTestModel.filename;
		const auto       matching_output     = temp_root / "matching-output.txt";
		fs::create_directories(matching_models_dir, ec);
		ok &= expect(!ec && write_file(matching_model, kTestModelContent),
		             "create matching installed test model");
		int matching_exit  = 0;
		download_succeeds  = true;
		downloaded_content = kTestModelContent;
		ok &=
		    expect(run_test_download({.models_dir = matching_models_dir, .output = matching_output},
		                             &matching_exit, test_models, successful_fake_download_file),
		           "run matching installed model");
		ok &= expect(matching_exit == 0 && attempted_downloads() == 0,
		             "matching installed model skips download");
		ok &= expect(read_file(matching_output).contains("Model already exists"),
		             "matching installed model reports already exists");

		int existing_hash_failure_exit = 0;
		ok &=
		    expect(run_test_download({.models_dir = matching_models_dir, .output = matching_output},
		                             &existing_hash_failure_exit, test_models,
		                             successful_fake_download_file, failing_sha256_file),
		           "run existing-model hash operation failure");
		const auto existing_hash_failure_stdout = read_file(matching_output);
		ok &= expect(existing_hash_failure_exit == EXIT_FAILURE && attempted_downloads() == 0,
		             "existing-model hash failure aborts before download");
		ok &= expect(read_file(matching_model) == kTestModelContent,
		             "existing-model hash failure preserves destination");
		ok &= expect(existing_hash_failure_stdout.contains("Failed to calculate SHA-256") &&
		                 !existing_hash_failure_stdout.contains("Model already exists") &&
		                 !existing_hash_failure_stdout.contains("Replacing invalid model"),
		             "existing-model hash failure reports operation failure only");

		failing_fstat_attempt           = 1;
		int existing_fstat_failure_exit = 0;
		ok &= expect(run_test_download(
		                 {.models_dir = matching_models_dir, .output = matching_output},
		                 &existing_fstat_failure_exit, test_models, successful_fake_download_file,
		                 howdy::native::download_models_internal::sha256_file_descriptor,
		                 selectively_failing_fstat),
		             "run existing-model fstat failure");
		const auto existing_fstat_failure_stdout = read_file(matching_output);
		ok &= expect(existing_fstat_failure_exit == EXIT_FAILURE && attempted_downloads() == 0,
		             "existing-model fstat failure aborts before download");
		ok &= expect(read_file(matching_model) == kTestModelContent,
		             "existing-model fstat failure preserves destination");
		ok &= expect(existing_fstat_failure_stdout.contains("fstat") &&
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
		ok &= expect(!ec && write_file(replacement_model, ""), "create empty installed model");
		int replacement_exit = 0;
		ok &= expect(
		    run_test_download({.models_dir = replacement_models_dir, .output = replacement_output},
		                      &replacement_exit, test_models, successful_fake_download_file),
		    "run empty replacement");
		const auto replacement_stdout = read_file(replacement_output);
		ok &= expect(replacement_exit == 0 && attempted_downloads() == 1 &&
		                 read_file(replacement_model) == kTestModelContent,
		             "empty installed model is replaced");
		ok &= expect(replacement_stdout.contains("Replacing invalid model download") &&
		                 !replacement_stdout.contains("Model already exists"),
		             "corrupted first model is never reported already existing");

		ok &= expect(write_file(replacement_model, "arbitrary existing data"),
		             "create arbitrary installed model");
		int arbitrary_exit = 0;
		ok &= expect(
		    run_test_download({.models_dir = replacement_models_dir, .output = replacement_output},
		                      &arbitrary_exit, test_models, successful_fake_download_file),
		    "run arbitrary replacement");
		ok &= expect(arbitrary_exit == 0 && attempted_downloads() == 1,
		             "arbitrary installed model triggers replacement");
		const auto arbitrary_stdout = read_file(replacement_output);
		ok &= expect(arbitrary_stdout.contains("Size mismatch") &&
		                 arbitrary_stdout.contains("expected 16, actual 23") &&
		                 !arbitrary_stdout.contains("Input/output error"),
		             "existing size mismatch reports expected and actual size only");

		ok &= expect(write_file(replacement_model, "small test modeL"),
		             "create same-size wrong-hash installed model");
		int wrong_hash_exit = 0;
		ok &= expect(
		    run_test_download({.models_dir = replacement_models_dir, .output = replacement_output},
		                      &wrong_hash_exit, test_models, successful_fake_download_file),
		    "run wrong-hash replacement");
		ok &= expect(wrong_hash_exit == 0 && attempted_downloads() == 1,
		             "wrong-hash installed model triggers replacement");

		ok &= expect(write_file(replacement_model, "old destination"),
		             "create replacement destination");
		downloaded_content       = "small test modeL";
		int staged_mismatch_exit = 0;
		ok &= expect(
		    run_test_download({.models_dir = replacement_models_dir, .output = replacement_output},
		                      &staged_mismatch_exit, test_models, successful_fake_download_file),
		    "run staged checksum mismatch");
		ok &= expect(staged_mismatch_exit == EXIT_FAILURE &&
		                 read_file(replacement_model) == "old destination",
		             "wrong staged hash preserves destination");
		ok &= expect(read_file(replacement_output).contains("Checksum mismatch"),
		             "same-size wrong staged hash reports checksum mismatch");
		ok &= expect(count_staged_files(replacement_models_dir, ".howdy-download-") == 0,
		             "wrong staged hash removes temporary file");

		ok &= expect(write_file(replacement_model, "old destination"),
		             "restore destination for staged size mismatch");
		downloaded_content   = "short";
		int staged_size_exit = 0;
		ok &= expect(
		    run_test_download({.models_dir = replacement_models_dir, .output = replacement_output},
		                      &staged_size_exit, test_models, successful_fake_download_file),
		    "run staged size mismatch");
		ok &= expect(staged_size_exit == EXIT_FAILURE &&
		                 read_file(replacement_model) == "old destination",
		             "staged size mismatch preserves destination");
		const auto staged_size_stdout = read_file(replacement_output);
		ok &= expect(staged_size_stdout.contains("Size mismatch") &&
		                 staged_size_stdout.contains("expected 16, actual 5") &&
		                 !staged_size_stdout.contains("Input/output error"),
		             "staged size mismatch reports expected and actual size only");

		ok &= expect(write_file(replacement_model, "small test modeL"),
		             "restore same-size invalid destination for staged fstat failure");
		downloaded_content    = kTestModelContent;
		failing_fstat_attempt = 2;
		int staged_fstat_exit = 0;
		ok &= expect(
		    run_test_download({.models_dir = replacement_models_dir, .output = replacement_output},
		                      &staged_fstat_exit, test_models, successful_fake_download_file,
		                      howdy::native::download_models_internal::sha256_file_descriptor,
		                      selectively_failing_fstat),
		    "run staged fstat failure");
		const auto staged_fstat_stdout = read_file(replacement_output);
		ok &= expect(staged_fstat_exit == EXIT_FAILURE &&
		                 read_file(replacement_model) == "small test modeL",
		             "staged fstat failure preserves destination");
		ok &= expect(staged_fstat_stdout.contains("fstat") &&
		                 staged_fstat_stdout.contains(replacement_models_dir.string()) &&
		                 staged_fstat_stdout.contains(".howdy-download-") &&
		                 staged_fstat_stdout.contains("Input/output error") &&
		                 !staged_fstat_stdout.contains("Size mismatch") &&
		                 !staged_fstat_stdout.contains("Checksum mismatch"),
		             "staged fstat failure reports staged path and syscall context only");
		ok &= expect(count_staged_files(replacement_models_dir, ".howdy-download-") == 0,
		             "staged fstat failure removes temporary file");

		ok &= expect(write_file(replacement_model, "old destination"),
		             "restore destination for hash read failure");
		int hash_read_failure_exit = 0;
		downloaded_content         = kTestModelContent;
		ok &= expect(
		    run_test_download({.models_dir = replacement_models_dir, .output = replacement_output},
		                      &hash_read_failure_exit, test_models, successful_fake_download_file,
		                      failing_sha256_file),
		    "run staged hash operation failure");
		ok &= expect(hash_read_failure_exit == EXIT_FAILURE &&
		                 read_file(replacement_model) == "old destination",
		             "hash read failure preserves destination");
		ok &= expect(read_file(replacement_output).contains("Failed to calculate SHA-256") &&
		                 !read_file(replacement_output).contains("Checksum mismatch"),
		             "hash read failure is distinct from digest mismatch");
		ok &= expect(count_staged_files(replacement_models_dir, ".howdy-download-") == 0,
		             "hash read failure removes temporary file");

		const auto insecure_models_dir = temp_root / "insecure-model";
		const auto insecure_model      = insecure_models_dir / kTestModel.filename;
		const auto insecure_output     = temp_root / "insecure-output.txt";
		fs::create_directories(insecure_models_dir, ec);
		ok &= expect(!ec && write_file(insecure_model, kTestModelContent) &&
		                 chmod(insecure_model.c_str(), 0664) == 0,
		             "create insecure installed model");
		int insecure_exit = 0;
		ok &=
		    expect(run_test_download({.models_dir = insecure_models_dir, .output = insecure_output},
		                             &insecure_exit, test_models, successful_fake_download_file),
		           "run insecure installed model");
		ok &= expect(insecure_exit == EXIT_FAILURE && attempted_downloads() == 0,
		             "insecure installed model aborts before download");

		downloaded_content = kTestModelContent;
		int installed_exit = 0;
		ok &= expect(
		    run_test_download({.models_dir = replacement_models_dir, .output = replacement_output},
		                      &installed_exit, test_models, successful_fake_download_file),
		    "run successful matching replacement");
		ok &= expect(installed_exit == 0 && read_file(replacement_model) == kTestModelContent,
		             "matching staged test model installs atomically");
		int second_run_exit = 0;
		ok &= expect(
		    run_test_download({.models_dir = replacement_models_dir, .output = replacement_output},
		                      &second_run_exit, test_models, successful_fake_download_file),
		    "run installed matching test model");
		ok &= expect(second_run_exit == 0 && attempted_downloads() == 0,
		             "second run performs zero downloads");
		return ok;
	}

}  // namespace howdy::test::download_models
