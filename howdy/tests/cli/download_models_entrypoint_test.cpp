#include "cli/download_models_test_support.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace howdy::test::download_models {

	using howdy::test::Expect;

	namespace {

		struct CurlSetoptCall {
			CURLoption                 option;
			CURLcode                   result;
			std::optional<long>        long_value;
			std::optional<curl_off_t>  off_t_value;
			std::optional<std::string> string_value;
		};

		struct CurlSetoptRecorder {
			std::optional<CURLoption>   failing_option;
			std::vector<CurlSetoptCall> calls;
		};

		auto SetoptResult(CurlSetoptRecorder &recorder, const CURLoption option) -> CURLcode {
			return recorder.failing_option.has_value() && *recorder.failing_option == option
			           ? CURLE_UNKNOWN_OPTION
			           : CURLE_OK;
		}

		auto FakeSetoptLong(void *context, CURL * /*curl*/, CURLoption option, long value)
		    -> CURLcode {
			auto *recorder = static_cast<CurlSetoptRecorder *>(context);
			if (recorder == nullptr) {
				return CURLE_FAILED_INIT;
			}
			const CURLcode result = SetoptResult(*recorder, option);
			recorder->calls.push_back({.option = option, .result = result, .long_value = value});
			return result;
		}

		auto FakeSetoptOffT(void *context, CURL * /*curl*/, CURLoption option, curl_off_t value)
		    -> CURLcode {
			auto *recorder = static_cast<CurlSetoptRecorder *>(context);
			if (recorder == nullptr) {
				return CURLE_FAILED_INIT;
			}
			const CURLcode result = SetoptResult(*recorder, option);
			recorder->calls.push_back({.option = option, .result = result, .off_t_value = value});
			return result;
		}

		auto FakeSetoptString(void *context, CURL * /*curl*/, CURLoption option, const char *value)
		    -> CURLcode {
			auto *recorder = static_cast<CurlSetoptRecorder *>(context);
			if (recorder == nullptr) {
				return CURLE_FAILED_INIT;
			}
			const CURLcode result = SetoptResult(*recorder, option);
			recorder->calls.push_back({.option = option, .result = result, .string_value = value});
			return result;
		}

		auto RecorderSetoptOperations(CurlSetoptRecorder &recorder)
		    -> howdy::native::download_models_internal::CurlSetoptOperations {
			return {.context    = &recorder,
			        .set_long   = FakeSetoptLong,
			        .set_off_t  = FakeSetoptOffT,
			        .set_string = FakeSetoptString};
		}

		auto ConfiguredLong(const CurlSetoptRecorder &recorder, CURLoption option, long value)
		    -> bool {
			return std::ranges::any_of(
			    recorder.calls, [option, value](const CurlSetoptCall &call) -> bool {
				    return call.option == option && call.result == CURLE_OK &&
				           call.long_value == value;
			    });
		}

		auto ConfiguredOffT(const CurlSetoptRecorder &recorder, CURLoption option, curl_off_t value)
		    -> bool {
			return std::ranges::any_of(
			    recorder.calls, [option, value](const CurlSetoptCall &call) -> bool {
				    return call.option == option && call.result == CURLE_OK &&
				           call.off_t_value == value;
			    });
		}

		auto ConfiguredString(const CurlSetoptRecorder &recorder, CURLoption option,
		                      std::string_view value) -> bool {
			return std::ranges::any_of(
			    recorder.calls, [option, value](const CurlSetoptCall &call) -> bool {
				    return call.option == option && call.result == CURLE_OK &&
				           call.string_value.has_value() && *call.string_value == value;
			    });
		}

		auto ConfiguredOption(const CurlSetoptRecorder &recorder, CURLoption option) -> bool {
			return std::ranges::any_of(recorder.calls,
			                           [option](const CurlSetoptCall &call) -> bool {
				                           return call.option == option;
			                           });
		}

	}  // namespace

	auto RunDownloadModelsEntrypointTests() -> bool {
		namespace fs = std::filesystem;

		bool            ok        = true;
		const auto      temp_root = fs::current_path() / "howdy-download-models-test";
		std::error_code ec;
		fs::remove_all(temp_root, ec);
		fs::create_directories(temp_root, ec);
		ok &= Expect(!ec, "create temp root");

		CurlSetoptRecorder successful_policy_recorder;
		ok &= Expect(howdy::native::download_models_internal::ConfigureTransferPolicy(
		                 nullptr, RecorderSetoptOperations(successful_policy_recorder)),
		             "transfer policy configures successfully");
		ok &= Expect(ConfiguredLong(successful_policy_recorder, CURLOPT_FOLLOWLOCATION, 1L),
		             "transfer policy follows redirects");
		ok &= Expect(ConfiguredLong(successful_policy_recorder, CURLOPT_SSL_VERIFYPEER, 1L),
		             "transfer policy verifies TLS peer");
		ok &= Expect(ConfiguredLong(successful_policy_recorder, CURLOPT_SSL_VERIFYHOST, 2L),
		             "transfer policy verifies TLS host");
		ok &= Expect(ConfiguredLong(successful_policy_recorder, CURLOPT_NOSIGNAL, 1L),
		             "transfer policy disables signals");
		ok &= Expect(ConfiguredLong(successful_policy_recorder, CURLOPT_CONNECTTIMEOUT, 15L),
		             "transfer policy configures connect timeout");
		ok &= Expect(ConfiguredLong(successful_policy_recorder, CURLOPT_TIMEOUT, 300L),
		             "transfer policy configures total timeout");
		ok &= Expect(ConfiguredLong(successful_policy_recorder, CURLOPT_LOW_SPEED_LIMIT, 1024L),
		             "transfer policy configures low-speed limit");
		ok &= Expect(ConfiguredLong(successful_policy_recorder, CURLOPT_LOW_SPEED_TIME, 30L),
		             "transfer policy configures low-speed timeout");
		ok &=
		    Expect(ConfiguredOffT(successful_policy_recorder, CURLOPT_MAXFILESIZE_LARGE,
		                          static_cast<curl_off_t>(
		                              howdy::native::download_models_internal::kMaxDownloadBytes)),
		           "transfer policy configures maximum download size");
		ok &= Expect(ConfiguredString(successful_policy_recorder, CURLOPT_PROTOCOLS_STR, "https"),
		             "transfer policy restricts initial URLs to HTTPS");
		ok &= Expect(
		    ConfiguredString(successful_policy_recorder, CURLOPT_REDIR_PROTOCOLS_STR, "https"),
		    "transfer policy restricts redirects to HTTPS");

		CurlSetoptRecorder initial_protocol_failure_recorder{
		    .failing_option = CURLOPT_PROTOCOLS_STR,
		};
		ok &= Expect(!howdy::native::download_models_internal::ConfigureTransferPolicy(
		                 nullptr, RecorderSetoptOperations(initial_protocol_failure_recorder)),
		             "initial HTTPS restriction failure aborts transfer policy");
		ok &= Expect(ConfiguredOption(initial_protocol_failure_recorder, CURLOPT_PROTOCOLS_STR),
		             "initial HTTPS restriction is attempted");
		ok &= Expect(
		    !ConfiguredString(initial_protocol_failure_recorder, CURLOPT_PROTOCOLS_STR, "https"),
		    "failed initial HTTPS restriction is not reported as configured");
		ok &= Expect(
		    !ConfiguredOption(initial_protocol_failure_recorder, CURLOPT_REDIR_PROTOCOLS_STR),
		    "initial HTTPS restriction failure skips redirect restriction");

		CurlSetoptRecorder redirect_protocol_failure_recorder{
		    .failing_option = CURLOPT_REDIR_PROTOCOLS_STR,
		};
		ok &= Expect(!howdy::native::download_models_internal::ConfigureTransferPolicy(
		                 nullptr, RecorderSetoptOperations(redirect_protocol_failure_recorder)),
		             "redirect HTTPS restriction failure aborts transfer policy");
		ok &= Expect(
		    ConfiguredOption(redirect_protocol_failure_recorder, CURLOPT_REDIR_PROTOCOLS_STR),
		    "redirect HTTPS restriction is attempted");
		ok &= Expect(!ConfiguredString(redirect_protocol_failure_recorder,
		                               CURLOPT_REDIR_PROTOCOLS_STR, "https"),
		             "failed redirect HTTPS restriction is not reported as configured");

		CurlSetoptRecorder existing_policy_failure_recorder{
		    .failing_option = CURLOPT_CONNECTTIMEOUT,
		};
		ok &= Expect(!howdy::native::download_models_internal::ConfigureTransferPolicy(
		                 nullptr, RecorderSetoptOperations(existing_policy_failure_recorder)),
		             "existing transfer policy failure aborts configuration");
		ok &= Expect(!ConfiguredOption(existing_policy_failure_recorder, CURLOPT_TIMEOUT),
		             "existing transfer policy failure short-circuits later options");

		CurlSetoptRecorder missing_callback_recorder;
		auto missing_callback_operations = RecorderSetoptOperations(missing_callback_recorder);
		missing_callback_operations.set_string = nullptr;
		ok &= Expect(!howdy::native::download_models_internal::ConfigureTransferPolicy(
		                 nullptr, missing_callback_operations),
		             "missing transfer policy callback fails closed");
		ok &= Expect(missing_callback_recorder.calls.empty(),
		             "missing transfer policy callback is not dereferenced");

		const auto null_download_models_dir = temp_root / "null-download-file" / "models";
		const auto null_download_output     = temp_root / "null-download-file-output.txt";
		ok &= Expect(!fs::exists(null_download_models_dir, ec) && !ec,
		             "null download callback models directory starts absent");
		int null_download_exit = 0;
		{
			EnvVarGuard models_env("HOWDY_MODELS_DIR", null_download_models_dir.string());
			ResetDependencyAttempts();
			ok &= Expect(CaptureDownloadModelsStdout(
			                 null_download_output, &null_download_exit,
			                 howdy::native::download_models_internal::DownloadModelsDependencies{
			                     .download_file        = nullptr,
			                     .model_file_owner_uid = TestModelFileOwnerUid,
			                 }),
			             "capture null download callback output");
		}
		ok &= Expect(null_download_exit == EXIT_FAILURE, "null download callback aborts");
		ok &= Expect(ReadFile(null_download_output).empty(),
		             "null download callback emits no output");
		ok &= Expect(AttemptedDownloads() == 0, "null download callback invokes no download");
		ok &= Expect(AttemptedOwnerUidLookups() == 0,
		             "null download callback invokes no owner lookup");
		ok &= Expect(!fs::exists(null_download_models_dir, ec) && !ec,
		             "null download callback creates no models directory");

		const auto null_owner_models_dir = temp_root / "null-owner-uid" / "models";
		const auto null_owner_output     = temp_root / "null-owner-uid-output.txt";
		ok &= Expect(!fs::exists(null_owner_models_dir, ec) && !ec,
		             "null owner callback models directory starts absent");
		int null_owner_exit = 0;
		{
			EnvVarGuard models_env("HOWDY_MODELS_DIR", null_owner_models_dir.string());
			ResetDependencyAttempts();
			ok &= Expect(CaptureDownloadModelsStdout(
			                 null_owner_output, &null_owner_exit,
			                 howdy::native::download_models_internal::DownloadModelsDependencies{
			                     .download_file        = FakeDownloadFile,
			                     .model_file_owner_uid = nullptr,
			                 }),
			             "capture null owner callback output");
		}
		ok &= Expect(null_owner_exit == EXIT_FAILURE, "null owner callback aborts");
		ok &= Expect(ReadFile(null_owner_output).empty(), "null owner callback emits no output");
		ok &= Expect(AttemptedDownloads() == 0, "null owner callback invokes no download");
		ok &=
		    Expect(AttemptedOwnerUidLookups() == 0, "null owner callback invokes no owner lookup");
		ok &= Expect(!fs::exists(null_owner_models_dir, ec) && !ec,
		             "null owner callback creates no models directory");

		const auto existing_models_dir = temp_root / "existing-models";
		const auto existing_output     = temp_root / "existing-output.txt";
		fs::create_directories(existing_models_dir, ec);
		ok &= Expect(!ec, "create existing models directory");
		ok &= Expect(chmod(existing_models_dir.c_str(), 0755) == 0,
		             "make existing models directory secure");

		int existing_exit = 0;
		ok &= Expect(
		    RunFirstDownloadAttempt({.models_dir = existing_models_dir, .output = existing_output},
		                            &existing_exit),
		    "capture existing-directory download-models output");
		const auto existing_stdout = ReadFile(existing_output);
		ok &=
		    Expect(existing_exit == EXIT_FAILURE, "stubbed download aborts after readiness passes");
		ok &= Expect(AttemptedDownloads() == 1,
		             "missing model in existing secure directory reaches download");
		ok &= Expect(existing_stdout.contains("Downloading face_detection_yunet_2026may.onnx"),
		             "existing secure directory starts first model download");
		ok &= Expect(CountFilesWithPrefix(existing_models_dir, ".howdy-download-") == 0,
		             "failed download removes staged file");

		const auto missing_parent_models_dir = temp_root / "missing-parent" / "models";
		const auto missing_parent_output     = temp_root / "missing-parent-output.txt";
		ok &= Expect(!fs::exists(missing_parent_models_dir, ec) && !ec,
		             "missing parent models directory starts absent");

		int missing_parent_exit = 0;
		ok &= Expect(RunFirstDownloadAttempt(
		                 {.models_dir = missing_parent_models_dir, .output = missing_parent_output},
		                 &missing_parent_exit),
		             "capture missing-parent download-models output");
		const auto missing_parent_stdout = ReadFile(missing_parent_output);
		ok &= Expect(missing_parent_exit == EXIT_FAILURE,
		             "stubbed download aborts after missing parent creation");
		ok &= Expect(fs::is_directory(missing_parent_models_dir, ec) && !ec,
		             "download-models creates missing models directory before readiness checks");
		ok &= Expect(AttemptedDownloads() == 1,
		             "missing model after parent creation reaches download");
		ok &=
		    Expect(missing_parent_stdout.contains("Downloading face_detection_yunet_2026may.onnx"),
		           "missing parent path starts first model download");

		const auto symlink_target     = temp_root / "symlink-target";
		const auto symlink_parent     = temp_root / "symlink-parent";
		const auto symlink_models_dir = symlink_parent / "models";
		const auto symlink_output     = temp_root / "symlink-output.txt";
		fs::create_directory(symlink_target, ec);
		ok &= Expect(!ec, "create symlink target directory");
		fs::remove(symlink_parent, ec);
		ec.clear();
		if (symlink(symlink_target.c_str(), symlink_parent.c_str()) == 0) {
			int symlink_exit = 0;
			ok &= Expect(
			    RunFirstDownloadAttempt(
			        {.models_dir = symlink_models_dir, .output = symlink_output}, &symlink_exit),
			    "capture symlink-parent download-models output");
			const auto symlink_stdout = ReadFile(symlink_output);
			ok &= Expect(symlink_exit == EXIT_FAILURE && AttemptedDownloads() == 0,
			             "symlink parent aborts before download");
			ok &= Expect(!fs::exists(symlink_models_dir, ec) && !ec,
			             "symlink parent is not used to create models directory");
			ok &= Expect(symlink_stdout.contains("must be a directory"),
			             "symlink parent reports insecure models directory");
		} else {
			std::cerr << "SKIP: symlink creation failed: " << std::strerror(errno) << "\n";
		}
		fs::remove(symlink_parent, ec);
		ec.clear();

		const auto unsafe_parent     = temp_root / "unsafe-parent";
		const auto unsafe_models_dir = unsafe_parent / "models";
		const auto unsafe_output     = temp_root / "unsafe-output.txt";
		fs::create_directory(unsafe_parent, ec);
		ok &= Expect(!ec && chmod(unsafe_parent.c_str(), 0777) == 0,
		             "create unsafe writable parent directory");
		int unsafe_parent_exit = 0;
		ok &= Expect(
		    RunFirstDownloadAttempt({.models_dir = unsafe_models_dir, .output = unsafe_output},
		                            &unsafe_parent_exit),
		    "capture unsafe-parent download-models output");
		const auto unsafe_parent_stdout = ReadFile(unsafe_output);
		ok &= Expect(unsafe_parent_exit == EXIT_FAILURE && AttemptedDownloads() == 0,
		             "unsafe parent aborts before download");
		ok &= Expect(!fs::exists(unsafe_models_dir, ec) && !ec,
		             "unsafe parent is not used to create models directory");
		ok &= Expect(unsafe_parent_stdout.contains("must not be group-writable") ||
		                 unsafe_parent_stdout.contains("must not be world-writable"),
		             "unsafe parent reports insecure models directory");
		ok &= Expect(chmod(unsafe_parent.c_str(), 0755) == 0, "restore unsafe parent mode");
		fs::remove_all(unsafe_parent, ec);
		ec.clear();

		const auto blocked_models_dir = temp_root / "blocked-models";
		const auto blocked_output     = temp_root / "blocked-output.txt";
		ok &= Expect(WriteFile(blocked_models_dir, "not a directory"),
		             "create file blocking models directory");

		int blocked_exit = 0;
		ok &=
		    Expect(RunFirstDownloadAttempt(
		               {.models_dir = blocked_models_dir, .output = blocked_output}, &blocked_exit),
		           "capture blocked models-dir download-models output");
		const auto blocked_stdout = ReadFile(blocked_output);
		ok &= Expect(blocked_exit == EXIT_FAILURE, "blocked models directory aborts cleanly");
		ok &= Expect(AttemptedDownloads() == 0,
		             "blocked models directory stops before first download");
		ok &= Expect(blocked_stdout.contains("Failed to create models directory:"),
		             "blocked models directory reports setup failure");
		return ok;
	}

}  // namespace howdy::test::download_models
