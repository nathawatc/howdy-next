#include "cli/download_models_internal.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

#include <sys/resource.h>
#include <sys/stat.h>

namespace {

	using howdy::test::expect;

	int                      download_attempts     = 0;
	int                      owner_uid_attempts    = 0;
	int                      fstat_attempts        = 0;
	int                      failing_fstat_attempt = 0;
	std::string              downloaded_content;
	std::vector<std::string> downloaded_urls;
	bool                     download_succeeds = false;

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

	constexpr auto kTestModelContent = "small test model";
	constexpr auto kTestModelSha256 =
	    "eceb1d87ddd7b5c0e1b63bdac2d086824ffd12eaa157b741835014618a1e6c24";
	constexpr howdy::native::OpenCvModelDescriptor kTestModel{
	    .type     = howdy::native::OpenCvModelType::kYunet,
	    .filename = "test-model.onnx",
	    .url      = "https://example.invalid/test-model.onnx",
	    .sha256   = kTestModelSha256,
	    .size     = std::string_view(kTestModelContent).size(),
	};

	struct PinnedModelArtifact {
		howdy::native::OpenCvModelType type;
		std::string_view               filename;
		std::string_view               url;
		std::string_view               sha256;
		std::uintmax_t                 size;
	};

	constexpr std::array kPinnedModelArtifacts = {
	    PinnedModelArtifact{
	        .type     = howdy::native::OpenCvModelType::kYunet,
	        .filename = "face_detection_yunet_2026may.onnx",
	        .url =
	            "https://github.com/opencv/opencv_zoo/raw/26cc381e4d2594bb9f47a26eb8fd96c94a13660d/"
	            "models/face_detection_yunet/face_detection_yunet_2026may.onnx",
	        .sha256 = "ebafce4e3c118d6554634be5c27ab333b4c047a9a8c3faf1d7cf93101c22f0f0",
	        .size   = 229738,
	    },
	    PinnedModelArtifact{
	        .type     = howdy::native::OpenCvModelType::kSface,
	        .filename = "face_recognition_sface_2021dec_int8.onnx",
	        .url =
	            "https://github.com/opencv/opencv_zoo/raw/088c3571ec70df15100a5e4c26894d95951e92e9/"
	            "models/face_recognition_sface/face_recognition_sface_2021dec_int8.onnx",
	        .sha256 = "2b0e941e6f16cc048c20aee0c8e31f569118f65d702914540f7bfdc14048d78a",
	        .size   = 9896933,
	    },
	};

	auto setopt_result(CurlSetoptRecorder &recorder, const CURLoption option) -> CURLcode {
		return recorder.failing_option.has_value() && *recorder.failing_option == option
		           ? CURLE_UNKNOWN_OPTION
		           : CURLE_OK;
	}

	auto fake_setopt_long(void *context, CURL * /*curl*/, CURLoption option, long value)
	    -> CURLcode {
		auto *recorder = static_cast<CurlSetoptRecorder *>(context);
		if (recorder == nullptr) {
			return CURLE_FAILED_INIT;
		}
		const CURLcode result = setopt_result(*recorder, option);
		recorder->calls.push_back({
		    .option     = option,
		    .result     = result,
		    .long_value = value,
		});
		return result;
	}

	auto fake_setopt_off_t(void *context, CURL * /*curl*/, CURLoption option, curl_off_t value)
	    -> CURLcode {
		auto *recorder = static_cast<CurlSetoptRecorder *>(context);
		if (recorder == nullptr) {
			return CURLE_FAILED_INIT;
		}
		const CURLcode result = setopt_result(*recorder, option);
		recorder->calls.push_back({
		    .option      = option,
		    .result      = result,
		    .off_t_value = value,
		});
		return result;
	}

	auto fake_setopt_string(void *context, CURL * /*curl*/, CURLoption option, const char *value)
	    -> CURLcode {
		auto *recorder = static_cast<CurlSetoptRecorder *>(context);
		if (recorder == nullptr) {
			return CURLE_FAILED_INIT;
		}
		const CURLcode result = setopt_result(*recorder, option);
		recorder->calls.push_back({
		    .option       = option,
		    .result       = result,
		    .string_value = value,
		});
		return result;
	}

	auto recorder_setopt_operations(CurlSetoptRecorder &recorder)
	    -> howdy::native::download_models_internal::CurlSetoptOperations {
		return {
		    .context    = &recorder,
		    .set_long   = fake_setopt_long,
		    .set_off_t  = fake_setopt_off_t,
		    .set_string = fake_setopt_string,
		};
	}

	auto configured_long(const CurlSetoptRecorder &recorder, CURLoption option, long value)
	    -> bool {
		return std::ranges::any_of(
		    recorder.calls, [option, value](const CurlSetoptCall &call) -> bool {
			    return call.option == option && call.result == CURLE_OK && call.long_value == value;
		    });
	}

	auto configured_off_t(const CurlSetoptRecorder &recorder, CURLoption option, curl_off_t value)
	    -> bool {
		return std::ranges::any_of(recorder.calls,
		                           [option, value](const CurlSetoptCall &call) -> bool {
			                           return call.option == option && call.result == CURLE_OK &&
			                                  call.off_t_value == value;
		                           });
	}

	auto configured_string(const CurlSetoptRecorder &recorder, CURLoption option,
	                       std::string_view value) -> bool {
		return std::ranges::any_of(
		    recorder.calls, [option, value](const CurlSetoptCall &call) -> bool {
			    return call.option == option && call.result == CURLE_OK &&
			           call.string_value.has_value() && *call.string_value == value;
		    });
	}

	auto configured_option(const CurlSetoptRecorder &recorder, CURLoption option) -> bool {
		return std::ranges::any_of(recorder.calls, [option](const CurlSetoptCall &call) -> bool {
			return call.option == option;
		});
	}

	void reset_dependency_attempts() {
		download_attempts  = 0;
		owner_uid_attempts = 0;
		fstat_attempts     = 0;
		downloaded_urls.clear();
	}

	auto attempted_downloads() -> int {
		return download_attempts;
	}

	auto attempted_owner_uid_lookups() -> int {
		return owner_uid_attempts;
	}

	auto fake_download_file(const std::string                                           &url,
	                        howdy::native::download_models_internal::StagedDownloadFile &staged)
	    -> bool {
		(void)url;
		(void)staged;
		++download_attempts;
		return false;
	}

	auto successful_fake_download_file(
	    const std::string &url, howdy::native::download_models_internal::StagedDownloadFile &staged)
	    -> bool {
		++download_attempts;
		downloaded_urls.push_back(url);
		return download_succeeds &&
		       howdy::native::write_all_to_fd(staged.fd.get(), downloaded_content);
	}

	auto failing_sha256_file(int /*fd*/) -> std::optional<std::string> {
		return std::nullopt;
	}

	auto selectively_failing_fstat(const int fd, struct stat *stat_buf) -> int {
		++fstat_attempts;
		if (fstat_attempts == failing_fstat_attempt) {
			errno = EIO;
			return -1;
		}
		return fstat(fd, stat_buf);
	}

	auto official_manifest_download_file(
	    const std::string &url, howdy::native::download_models_internal::StagedDownloadFile &staged)
	    -> bool {
		++download_attempts;
		downloaded_urls.push_back(url);
		for (const auto &artifact : kPinnedModelArtifacts) {
			if (artifact.url == url) {
				return ftruncate(staged.fd.get(), static_cast<off_t>(artifact.size)) == 0;
			}
		}
		return false;
	}

	auto official_manifest_sha256_file(const int fd) -> std::optional<std::string> {
		struct stat stat_buf{};
		if (fstat(fd, &stat_buf) != 0 || stat_buf.st_size < 0) {
			return std::nullopt;
		}
		for (const auto &artifact : kPinnedModelArtifacts) {
			if (std::cmp_equal(stat_buf.st_size, artifact.size)) {
				return std::string(artifact.sha256);
			}
		}
		return std::nullopt;
	}

	auto test_model_file_owner_uid() -> std::optional<uid_t> {
		++owner_uid_attempts;
		return std::nullopt;
	}

	auto read_file(const std::filesystem::path &path) -> std::string {
		std::ifstream in(path);
		if (!in.is_open()) {
			return {};
		}
		return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
	}

	auto write_file(const std::filesystem::path &path, const std::string &content) -> bool {
		std::ofstream out(path);
		if (!out.is_open()) {
			return false;
		}
		out << content;
		return out.good();
	}

	auto count_staged_files(const std::filesystem::path &directory, std::string_view prefix)
	    -> std::size_t {
		std::size_t count = 0;
		for (const auto &entry : std::filesystem::directory_iterator(directory)) {
			if (entry.path().filename().string().starts_with(prefix)) {
				++count;
			}
		}
		return count;
	}

	auto commit_is_durable(howdy::native::AtomicFileCommitResult result) -> bool {
		return howdy::native::atomic_file_commit_is_durable(result);
	}

	auto fail_parent_sync(const std::filesystem::path & /*path*/) -> bool {
		return false;
	}

	auto get_env_value(const char *name) -> std::optional<std::string> {
		const char *value = std::getenv(name);
		if (value == nullptr) {
			return std::nullopt;
		}
		return std::string(value);
	}

	struct EnvVarGuard {
		const char                *name;
		std::optional<std::string> previous;

		EnvVarGuard(const char *env_name, const std::string &value)
		    : name(env_name)
		    , previous(get_env_value(env_name)) {
			setenv(name, value.c_str(), 1);
		}

		EnvVarGuard(const EnvVarGuard &)                     = delete;
		auto operator=(const EnvVarGuard &) -> EnvVarGuard & = delete;

		~EnvVarGuard() {
			if (previous.has_value()) {
				setenv(name, previous->c_str(), 1);
				return;
			}
			unsetenv(name);
		}
	};

	struct UmaskGuard {
		mode_t previous;

		explicit UmaskGuard(mode_t value)
		    : previous(umask(value)) {}

		UmaskGuard(const UmaskGuard &)                     = delete;
		auto operator=(const UmaskGuard &) -> UmaskGuard & = delete;

		~UmaskGuard() {
			umask(previous);
		}
	};

	struct StdoutRedirectGuard {
		int  saved_stdout = -1;
		bool active       = false;

		explicit StdoutRedirectGuard(const std::filesystem::path &path)
		    : saved_stdout(dup(STDOUT_FILENO)) {
			if (saved_stdout < 0) {
				return;
			}

			const int output_fd =
			    open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
			if (output_fd < 0) {
				close(saved_stdout);
				saved_stdout = -1;
				return;
			}

			if (dup2(output_fd, STDOUT_FILENO) < 0) {
				close(output_fd);
				close(saved_stdout);
				saved_stdout = -1;
				return;
			}
			close(output_fd);
			active = true;
		}

		StdoutRedirectGuard(const StdoutRedirectGuard &)                     = delete;
		auto operator=(const StdoutRedirectGuard &) -> StdoutRedirectGuard & = delete;

		~StdoutRedirectGuard() {
			if (saved_stdout >= 0) {
				std::cout.flush();
				dup2(saved_stdout, STDOUT_FILENO);
				close(saved_stdout);
			}
		}

		[[nodiscard]] auto ok() const -> bool {
			return active;
		}
	};

	auto capture_download_models_stdout(
	    const std::filesystem::path &path, int *exit_code,
	    const howdy::native::download_models_internal::DownloadModelsDependencies &dependencies)
	    -> bool {
		StdoutRedirectGuard stdout_redirect(path);
		if (!stdout_redirect.ok()) {
			return false;
		}

		std::array<char *, 2> argv = {
		    const_cast<char *>("howdy-download-models"),
		    nullptr,
		};
		*exit_code =
		    howdy::native::download_models_internal::download_models_main_with_dependencies(
		        1, argv.data(), dependencies);
		return true;
	}

	struct DownloadPaths {
		std::filesystem::path models_dir;
		std::filesystem::path output;
	};

	auto
	run_test_download(const DownloadPaths &paths, int *exit_code,
	                  const std::span<const howdy::native::OpenCvModelDescriptor>   models,
	                  const howdy::native::download_models_internal::DownloadFileFn download_file,
	                  const howdy::native::download_models_internal::Sha256FileFn   sha256_file =
	                      howdy::native::download_models_internal::sha256_file_descriptor,
	                  const howdy::native::download_models_internal::FstatFn fstat_file = ::fstat)
	    -> bool {
		EnvVarGuard models_env("HOWDY_MODELS_DIR", paths.models_dir.string());
		reset_dependency_attempts();
		return capture_download_models_stdout(
		    paths.output, exit_code,
		    howdy::native::download_models_internal::DownloadModelsDependencies{
		        .download_file        = download_file,
		        .model_file_owner_uid = test_model_file_owner_uid,
		        .sha256_file          = sha256_file,
		        .fstat_file           = fstat_file,
		        .models               = models,
		    });
	}

	auto run_first_download_attempt(const DownloadPaths &paths, int *exit_code) -> bool {
		EnvVarGuard models_env("HOWDY_MODELS_DIR", paths.models_dir.string());
		reset_dependency_attempts();
		return capture_download_models_stdout(
		    paths.output, exit_code,
		    howdy::native::download_models_internal::DownloadModelsDependencies{
		        .download_file        = fake_download_file,
		        .model_file_owner_uid = test_model_file_owner_uid,
		    });
	}

	template <typename Value, typename Callback>
	void with_present(std::optional<Value> &value, Callback callback) {
		if (value.has_value()) {
			callback();
		}
	}

}  // namespace

namespace {

	auto test_download_entrypoints() -> bool {
		namespace fs = std::filesystem;

		bool            ok        = true;
		const auto      temp_root = fs::current_path() / "howdy-download-models-test";
		std::error_code ec;
		fs::remove_all(temp_root, ec);
		fs::create_directories(temp_root, ec);
		ok &= expect(!ec, "create temp root");

		CurlSetoptRecorder successful_policy_recorder;
		ok &= expect(howdy::native::download_models_internal::configure_transfer_policy(
		                 nullptr, recorder_setopt_operations(successful_policy_recorder)),
		             "transfer policy configures successfully");
		ok &= expect(configured_long(successful_policy_recorder, CURLOPT_FOLLOWLOCATION, 1L),
		             "transfer policy follows redirects");
		ok &= expect(configured_long(successful_policy_recorder, CURLOPT_NOSIGNAL, 1L),
		             "transfer policy disables signals");
		ok &= expect(configured_long(successful_policy_recorder, CURLOPT_CONNECTTIMEOUT, 15L),
		             "transfer policy configures connect timeout");
		ok &= expect(configured_long(successful_policy_recorder, CURLOPT_TIMEOUT, 300L),
		             "transfer policy configures total timeout");
		ok &= expect(configured_long(successful_policy_recorder, CURLOPT_LOW_SPEED_LIMIT, 1024L),
		             "transfer policy configures low-speed limit");
		ok &= expect(configured_long(successful_policy_recorder, CURLOPT_LOW_SPEED_TIME, 30L),
		             "transfer policy configures low-speed timeout");
		ok &= expect(
		    configured_off_t(successful_policy_recorder, CURLOPT_MAXFILESIZE_LARGE,
		                     static_cast<curl_off_t>(
		                         howdy::native::download_models_internal::kMaxDownloadBytes)),
		    "transfer policy configures maximum download size");
		ok &= expect(configured_string(successful_policy_recorder, CURLOPT_PROTOCOLS_STR, "https"),
		             "transfer policy restricts initial URLs to HTTPS");
		ok &= expect(
		    configured_string(successful_policy_recorder, CURLOPT_REDIR_PROTOCOLS_STR, "https"),
		    "transfer policy restricts redirects to HTTPS");

		CurlSetoptRecorder initial_protocol_failure_recorder{
		    .failing_option = CURLOPT_PROTOCOLS_STR,
		};
		ok &= expect(!howdy::native::download_models_internal::configure_transfer_policy(
		                 nullptr, recorder_setopt_operations(initial_protocol_failure_recorder)),
		             "initial HTTPS restriction failure aborts transfer policy");
		ok &= expect(configured_option(initial_protocol_failure_recorder, CURLOPT_PROTOCOLS_STR),
		             "initial HTTPS restriction is attempted");
		ok &= expect(
		    !configured_string(initial_protocol_failure_recorder, CURLOPT_PROTOCOLS_STR, "https"),
		    "failed initial HTTPS restriction is not reported as configured");
		ok &= expect(
		    !configured_option(initial_protocol_failure_recorder, CURLOPT_REDIR_PROTOCOLS_STR),
		    "initial HTTPS restriction failure skips redirect restriction");

		CurlSetoptRecorder redirect_protocol_failure_recorder{
		    .failing_option = CURLOPT_REDIR_PROTOCOLS_STR,
		};
		ok &= expect(!howdy::native::download_models_internal::configure_transfer_policy(
		                 nullptr, recorder_setopt_operations(redirect_protocol_failure_recorder)),
		             "redirect HTTPS restriction failure aborts transfer policy");
		ok &= expect(
		    configured_option(redirect_protocol_failure_recorder, CURLOPT_REDIR_PROTOCOLS_STR),
		    "redirect HTTPS restriction is attempted");
		ok &= expect(!configured_string(redirect_protocol_failure_recorder,
		                                CURLOPT_REDIR_PROTOCOLS_STR, "https"),
		             "failed redirect HTTPS restriction is not reported as configured");

		CurlSetoptRecorder existing_policy_failure_recorder{
		    .failing_option = CURLOPT_CONNECTTIMEOUT,
		};
		ok &= expect(!howdy::native::download_models_internal::configure_transfer_policy(
		                 nullptr, recorder_setopt_operations(existing_policy_failure_recorder)),
		             "existing transfer policy failure aborts configuration");
		ok &= expect(!configured_option(existing_policy_failure_recorder, CURLOPT_TIMEOUT),
		             "existing transfer policy failure short-circuits later options");

		CurlSetoptRecorder missing_callback_recorder;
		auto missing_callback_operations = recorder_setopt_operations(missing_callback_recorder);
		missing_callback_operations.set_string = nullptr;
		ok &= expect(!howdy::native::download_models_internal::configure_transfer_policy(
		                 nullptr, missing_callback_operations),
		             "missing transfer policy callback fails closed");
		ok &= expect(missing_callback_recorder.calls.empty(),
		             "missing transfer policy callback is not dereferenced");

		const auto null_download_models_dir = temp_root / "null-download-file" / "models";
		const auto null_download_output     = temp_root / "null-download-file-output.txt";
		ok &= expect(!fs::exists(null_download_models_dir, ec) && !ec,
		             "null download callback models directory starts absent");
		int null_download_exit = 0;
		{
			EnvVarGuard models_env("HOWDY_MODELS_DIR", null_download_models_dir.string());
			reset_dependency_attempts();
			ok &= expect(capture_download_models_stdout(
			                 null_download_output, &null_download_exit,
			                 howdy::native::download_models_internal::DownloadModelsDependencies{
			                     .download_file        = nullptr,
			                     .model_file_owner_uid = test_model_file_owner_uid,
			                 }),
			             "capture null download callback output");
		}
		ok &= expect(null_download_exit == EXIT_FAILURE, "null download callback aborts");
		ok &= expect(read_file(null_download_output).empty(),
		             "null download callback emits no output");
		ok &= expect(attempted_downloads() == 0, "null download callback invokes no download");
		ok &= expect(attempted_owner_uid_lookups() == 0,
		             "null download callback invokes no owner lookup");
		ok &= expect(!fs::exists(null_download_models_dir, ec) && !ec,
		             "null download callback creates no models directory");

		const auto null_owner_models_dir = temp_root / "null-owner-uid" / "models";
		const auto null_owner_output     = temp_root / "null-owner-uid-output.txt";
		ok &= expect(!fs::exists(null_owner_models_dir, ec) && !ec,
		             "null owner callback models directory starts absent");
		int null_owner_exit = 0;
		{
			EnvVarGuard models_env("HOWDY_MODELS_DIR", null_owner_models_dir.string());
			reset_dependency_attempts();
			ok &= expect(capture_download_models_stdout(
			                 null_owner_output, &null_owner_exit,
			                 howdy::native::download_models_internal::DownloadModelsDependencies{
			                     .download_file        = fake_download_file,
			                     .model_file_owner_uid = nullptr,
			                 }),
			             "capture null owner callback output");
		}
		ok &= expect(null_owner_exit == EXIT_FAILURE, "null owner callback aborts");
		ok &= expect(read_file(null_owner_output).empty(), "null owner callback emits no output");
		ok &= expect(attempted_downloads() == 0, "null owner callback invokes no download");
		ok &= expect(attempted_owner_uid_lookups() == 0,
		             "null owner callback invokes no owner lookup");
		ok &= expect(!fs::exists(null_owner_models_dir, ec) && !ec,
		             "null owner callback creates no models directory");

		const auto existing_models_dir = temp_root / "existing-models";
		const auto existing_output     = temp_root / "existing-output.txt";
		fs::create_directories(existing_models_dir, ec);
		ok &= expect(!ec, "create existing models directory");
		ok &= expect(chmod(existing_models_dir.c_str(), 0755) == 0,
		             "make existing models directory secure");

		int existing_exit = 0;
		ok &= expect(
		    run_first_download_attempt(
		        {.models_dir = existing_models_dir, .output = existing_output}, &existing_exit),
		    "capture existing-directory download-models output");
		const auto existing_stdout = read_file(existing_output);
		ok &=
		    expect(existing_exit == EXIT_FAILURE, "stubbed download aborts after readiness passes");
		ok &= expect(attempted_downloads() == 1,
		             "missing model in existing secure directory reaches download");
		ok &= expect(existing_stdout.contains("Downloading face_detection_yunet_2026may.onnx"),
		             "existing secure directory starts first model download");
		ok &= expect(count_staged_files(existing_models_dir, ".howdy-download-") == 0,
		             "failed download removes staged file");

		const auto missing_parent_models_dir = temp_root / "missing-parent" / "models";
		const auto missing_parent_output     = temp_root / "missing-parent-output.txt";
		ok &= expect(!fs::exists(missing_parent_models_dir, ec) && !ec,
		             "missing parent models directory starts absent");

		int missing_parent_exit = 0;
		ok &= expect(run_first_download_attempt(
		                 {.models_dir = missing_parent_models_dir, .output = missing_parent_output},
		                 &missing_parent_exit),
		             "capture missing-parent download-models output");
		const auto missing_parent_stdout = read_file(missing_parent_output);
		ok &= expect(missing_parent_exit == EXIT_FAILURE,
		             "stubbed download aborts after missing parent creation");
		ok &= expect(fs::is_directory(missing_parent_models_dir, ec) && !ec,
		             "download-models creates missing models directory before readiness checks");
		ok &= expect(attempted_downloads() == 1,
		             "missing model after parent creation reaches download");
		ok &=
		    expect(missing_parent_stdout.contains("Downloading face_detection_yunet_2026may.onnx"),
		           "missing parent path starts first model download");

		const auto blocked_models_dir = temp_root / "blocked-models";
		const auto blocked_output     = temp_root / "blocked-output.txt";
		ok &= expect(write_file(blocked_models_dir, "not a directory"),
		             "create file blocking models directory");

		int blocked_exit = 0;
		ok &=
		    expect(run_first_download_attempt(
		               {.models_dir = blocked_models_dir, .output = blocked_output}, &blocked_exit),
		           "capture blocked models-dir download-models output");
		const auto blocked_stdout = read_file(blocked_output);
		ok &= expect(blocked_exit == EXIT_FAILURE, "blocked models directory aborts cleanly");
		ok &= expect(attempted_downloads() == 0,
		             "blocked models directory stops before first download");
		ok &= expect(blocked_stdout.contains("Failed to create models directory:"),
		             "blocked models directory reports setup failure");
		return ok;
	}

	auto test_atomic_files() -> bool {
		namespace fs              = std::filesystem;
		bool            ok        = true;
		const auto      temp_root = fs::current_path() / "howdy-download-models-test";
		std::error_code ec;

		howdy::native::StagedFile empty_path_install{
		    .fd   = howdy::native::ScopedFd(open("/dev/null", O_WRONLY)),
		    .path = {},
		};
		ok &= expect(empty_path_install.fd.get() >= 0, "open fd for empty-path staged install");
		ok &= expect(!commit_is_durable(howdy::native::install_staged_file(empty_path_install,
		                                                                   temp_root / "unused")),
		             "staged install rejects empty path");
		ok &= expect(empty_path_install.fd.get() < 0, "empty-path staged install closes fd");

		const auto blocked_parent = temp_root / "blocked-parent";
		ok &= expect(write_file(blocked_parent, "not a directory"),
		             "create file blocking staged parent directory");
		ok &= expect(!howdy::native::prepare_staged_file(blocked_parent / "models" / "model.onnx",
		                                                 ".howdy-download-")
		                  .has_value(),
		             "prepare staged file returns nullopt when parent creation fails");
		ok &= expect(!howdy::native::sync_parent_directory(blocked_parent / "model.onnx"),
		             "parent-directory sync reports open failure");

		const auto overflow_destination = temp_root / "overflow-write.onnx";
		auto       overflow_staged =
		    howdy::native::prepare_staged_file(overflow_destination, ".howdy-download-");
		ok &=
		    expect(overflow_staged.has_value(), "prepare staged file for overflow write callback");
		with_present(overflow_staged, [&] -> void {
			howdy::native::download_models_internal::DownloadWriteContext write_context{
			    .staged = &*overflow_staged,
			};
			std::array<char, 2> payload        = {'x', 'y'};
			const auto          overflow_size  = (std::numeric_limits<std::size_t>::max() / 2) + 2;
			const auto          overflow_count = static_cast<std::size_t>(2);
			const auto          result =
			    howdy::native::download_models_internal::download_models_write_callback(
			        payload.data(), overflow_size, overflow_count, &write_context);
			ok &= expect(result == 0, "overflowing write callback returns 0");
			ok &= expect(read_file(overflow_staged->path).empty(),
			             "overflowing write callback leaves staged file empty");
			howdy::native::cleanup_staged_file(*overflow_staged);
		});

		const auto limited_destination = temp_root / "limited-write.onnx";
		auto       limited_staged =
		    howdy::native::prepare_staged_file(limited_destination, ".howdy-download-");
		ok &= expect(limited_staged.has_value(), "prepare staged file for cumulative write limit");
		with_present(limited_staged, [&] -> void {
			howdy::native::download_models_internal::DownloadWriteContext write_context{
			    .staged    = &*limited_staged,
			    .max_bytes = 3,
			};
			std::array<char, 2> first_payload  = {'x', 'y'};
			std::array<char, 2> second_payload = {'z', '!'};
			const auto          first_result =
			    howdy::native::download_models_internal::download_models_write_callback(
			        first_payload.data(), 1, first_payload.size(), &write_context);
			const auto second_result =
			    howdy::native::download_models_internal::download_models_write_callback(
			        second_payload.data(), 1, second_payload.size(), &write_context);
			ok &= expect(first_result == first_payload.size(),
			             "write callback accepts data within cumulative limit");
			ok &= expect(second_result == 0,
			             "write callback rejects data exceeding cumulative limit");
			ok &= expect(write_context.bytes_written == first_payload.size(),
			             "write callback tracks only committed bytes");
			ok &= expect(read_file(limited_staged->path) == "xy",
			             "write callback leaves rejected chunk unwritten");
			howdy::native::cleanup_staged_file(*limited_staged);
		});

		const auto successful_install_destination = temp_root / "successful-install.onnx";
		auto       successful_install =
		    howdy::native::prepare_staged_file(successful_install_destination, ".howdy-download-");
		ok &= expect(successful_install.has_value(), "prepare staged file for successful install");
		with_present(successful_install, [&] -> void {
			const auto staged_path = successful_install->path;
			ok &= expect(howdy::native::write_all_to_fd(successful_install->fd.get(), "model"),
			             "download-like write leaves staged fd open before install");
			ok &= expect(successful_install->fd.get() >= 0,
			             "download-like staged fd remains open until install");
			ok &= expect(commit_is_durable(howdy::native::install_staged_file(
			                 *successful_install, successful_install_destination)),
			             "install staged file fsyncs and closes download-like open fd");
			ok &= expect(successful_install->fd.get() < 0, "successful install closes staged fd");
			ok &= expect(successful_install->path.empty(), "successful install clears staged path");
			ok &= expect(fs::exists(successful_install_destination, ec) && !ec,
			             "successful install creates destination");
			ok &= expect(read_file(successful_install_destination) == "model",
			             "successful install preserves staged content");
			ok &= expect(!fs::exists(staged_path, ec) && !ec,
			             "successful install removes staged temp path");
		});

		const auto uncertain_install_destination = temp_root / "uncertain-install.onnx";
		auto       uncertain_install =
		    howdy::native::prepare_staged_file(uncertain_install_destination, ".howdy-download-");
		ok &= expect(uncertain_install.has_value(), "prepare staged file for uncertain install");
		with_present(uncertain_install, [&] -> void {
			ok &= expect(howdy::native::write_all_to_fd(uncertain_install->fd.get(), "model"),
			             "write uncertain staged install content");
			const auto install_result = howdy::native::install_staged_file(
			    *uncertain_install, uncertain_install_destination, fail_parent_sync);
			ok &= expect(install_result ==
			                 howdy::native::AtomicFileCommitResult::kCommittedSyncFailed,
			             "failed parent sync reports committed install with uncertain durability");
			ok &= expect(read_file(uncertain_install_destination) == "model",
			             "failed parent sync leaves committed destination visible");
			ok &= expect(uncertain_install->path.empty(),
			             "failed parent sync clears consumed staged path");
		});

		const auto failed_install_destination = temp_root / "failed-install.onnx";
		auto       failed_install =
		    howdy::native::prepare_staged_file(failed_install_destination, ".howdy-download-");
		ok &= expect(failed_install.has_value(), "prepare staged file for failed install");
		with_present(failed_install, [&] -> void {
			const auto staged_path = failed_install->path;
			ok &= expect(!commit_is_durable(howdy::native::install_staged_file(
			                 *failed_install, temp_root / "missing" / "model")),
			             "staged install fails for missing destination parent");
			ok &= expect(!fs::exists(staged_path, ec) && !ec,
			             "failed staged install removes staged file");
		});

		const auto atomic_new_path = temp_root / "atomic" / "new-file";
		ok &= expect(commit_is_durable(howdy::native::write_atomic_file(
		                 atomic_new_path, "new content", S_IRUSR | S_IWUSR)),
		             "atomic writer creates new file");
		ok &= expect(read_file(atomic_new_path) == "new content",
		             "atomic writer preserves new content");
		struct stat atomic_new_stat{};
		ok &= expect(stat(atomic_new_path.c_str(), &atomic_new_stat) == 0,
		             "stat atomic writer new file");
		ok &= expect(S_ISREG(atomic_new_stat.st_mode), "atomic writer new target is regular file");
		ok &= expect((atomic_new_stat.st_mode & 07777) == 0600,
		             "atomic writer applies requested new-file mode");
		ok &= expect(count_staged_files(atomic_new_path.parent_path(), ".howdy-atomic-") == 0,
		             "atomic writer removes staged file after new-file install");

		const auto atomic_existing_path = temp_root / "atomic-existing";
		ok &= expect(write_file(atomic_existing_path, "initial"),
		             "create existing atomic writer target");
		ok &= expect(chmod(atomic_existing_path.c_str(), 0640) == 0,
		             "set existing atomic writer target mode");
		ok &= expect(commit_is_durable(howdy::native::write_atomic_file(atomic_existing_path,
		                                                                "replacement", 0600)),
		             "atomic writer replaces existing file");
		ok &= expect(read_file(atomic_existing_path) == "replacement",
		             "atomic writer preserves replacement content");
		struct stat atomic_existing_stat{};
		ok &= expect(stat(atomic_existing_path.c_str(), &atomic_existing_stat) == 0,
		             "stat replaced atomic writer file");
		ok &= expect((atomic_existing_stat.st_mode & 07777) == 0640,
		             "atomic writer preserves existing-file mode");
		ok &= expect(count_staged_files(atomic_existing_path.parent_path(), ".howdy-atomic-") == 0,
		             "atomic writer removes staged file after replacement");

		const auto default_mode_path = temp_root / "default-mode-existing";
		ok &= expect(write_file(default_mode_path, "old"), "create default-mode policy target");
		ok &= expect(chmod(default_mode_path.c_str(), 0600) == 0,
		             "set default-mode policy target mode");
		auto default_mode_staged = howdy::native::prepare_staged_file(
		    default_mode_path, ".howdy-default-mode-", S_IRUSR | S_IWUSR | S_IRGRP,
		    howdy::native::StagedFileMetadataPolicy::kUseDefaultMode);
		ok &= expect(default_mode_staged.has_value(), "prepare default-mode policy staged file");
		with_present(default_mode_staged, [&] -> void {
			ok &= expect(howdy::native::write_all_to_fd(default_mode_staged->fd.get(), "new"),
			             "write default-mode policy staged file");
			ok &= expect(commit_is_durable(howdy::native::install_staged_file(*default_mode_staged,
			                                                                  default_mode_path)),
			             "install default-mode policy staged file");
			ok &= expect(read_file(default_mode_path) == "new",
			             "default-mode policy installs replacement content");
			struct stat default_mode_stat{};
			ok &= expect(stat(default_mode_path.c_str(), &default_mode_stat) == 0,
			             "stat default-mode policy target");
			ok &= expect((default_mode_stat.st_mode & 07777) == 0640,
			             "default-mode policy applies requested mode to existing file");
			ok &= expect(
			    count_staged_files(default_mode_path.parent_path(), ".howdy-default-mode-") == 0,
			    "default-mode policy removes staged file after replacement");
		});

		const auto atomic_directory_target = temp_root / "atomic-directory-target";
		fs::create_directory(atomic_directory_target, ec);
		ok &= expect(!ec, "create directory atomic writer target");
		ok &= expect(!commit_is_durable(howdy::native::write_atomic_file(atomic_directory_target,
		                                                                 "must not replace")),
		             "atomic writer rejects directory target");
		ok &= expect(fs::is_directory(atomic_directory_target, ec) && !ec,
		             "atomic writer leaves directory target unchanged");
		ok &=
		    expect(count_staged_files(atomic_directory_target.parent_path(), ".howdy-atomic-") == 0,
		           "atomic writer creates no staged file for directory target");

		const auto atomic_blocked_parent = temp_root / "atomic-blocked-parent";
		ok &= expect(write_file(atomic_blocked_parent, "blocking content"),
		             "create file blocking atomic writer parent");
		const auto atomic_blocked_path = atomic_blocked_parent / "child" / "file";
		ok &= expect(!commit_is_durable(
		                 howdy::native::write_atomic_file(atomic_blocked_path, "must not write")),
		             "atomic writer rejects blocked parent path");
		ok &= expect(!fs::exists(atomic_blocked_parent / "child", ec) && !ec,
		             "atomic writer creates no child under blocked parent");
		ok &= expect(read_file(atomic_blocked_parent) == "blocking content",
		             "atomic writer preserves blocking file content");

		const auto atomic_write_failure_path = temp_root / "atomic-write-failure";
		ok &= expect(write_file(atomic_write_failure_path, "original content"),
		             "create atomic writer write-failure target");
		struct rlimit original_file_size_limit{};
		const bool have_file_size_limit = getrlimit(RLIMIT_FSIZE, &original_file_size_limit) == 0;
		ok &= expect(have_file_size_limit, "read file-size resource limit");
		const auto previous_sigxfsz_handler = std::signal(SIGXFSZ, SIG_IGN);
		const bool have_sigxfsz_handler     = previous_sigxfsz_handler != SIG_ERR;
		ok &= expect(have_sigxfsz_handler, "ignore file-size signal for failed atomic write");
		bool atomic_write_failed = false;
		if (have_file_size_limit && have_sigxfsz_handler) {
			auto zero_file_size_limit     = original_file_size_limit;
			zero_file_size_limit.rlim_cur = 0;
			const bool limited_file_size  = setrlimit(RLIMIT_FSIZE, &zero_file_size_limit) == 0;
			ok &= expect(limited_file_size, "limit file size for failed atomic write");
			if (limited_file_size) {
				atomic_write_failed = !commit_is_durable(
				    howdy::native::write_atomic_file(atomic_write_failure_path, "replacement"));
				ok &= expect(setrlimit(RLIMIT_FSIZE, &original_file_size_limit) == 0,
				             "restore file-size resource limit");
			}
		}
		if (have_sigxfsz_handler) {
			ok &= expect(std::signal(SIGXFSZ, previous_sigxfsz_handler) != SIG_ERR,
			             "restore file-size signal handler");
		}
		ok &= expect(atomic_write_failed, "atomic writer reports staged content write failure");
		ok &= expect(read_file(atomic_write_failure_path) == "original content",
		             "failed atomic write preserves existing target");
		ok &= expect(
		    count_staged_files(atomic_write_failure_path.parent_path(), ".howdy-atomic-") == 0,
		    "failed atomic write removes staged file");
		return ok;
	}

	auto test_model_downloads() -> bool {
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

	auto test_official_manifest() -> bool {
		namespace fs              = std::filesystem;
		bool            ok        = true;
		const auto      temp_root = fs::current_path() / "howdy-download-models-test";
		std::error_code ec;

		const auto manifest_models_dir = temp_root / "manifest-models";
		const auto manifest_output     = temp_root / "manifest-output.txt";
		int        manifest_exit       = 0;
		ok &= expect(
		    run_test_download({.models_dir = manifest_models_dir, .output = manifest_output},
		                      &manifest_exit, howdy::native::official_opencv_models(),
		                      official_manifest_download_file, official_manifest_sha256_file),
		    "run official manifest downloads");
		std::error_code yunet_size_ec;
		std::error_code sface_size_ec;
		ok &= expect(manifest_exit == 0 && downloaded_urls.size() == kPinnedModelArtifacts.size() &&
		                 downloaded_urls[0] == kPinnedModelArtifacts[0].url &&
		                 downloaded_urls[1] == kPinnedModelArtifacts[1].url &&
		                 fs::file_size(manifest_models_dir / kPinnedModelArtifacts[0].filename,
		                               yunet_size_ec) == kPinnedModelArtifacts[0].size &&
		                 !yunet_size_ec &&
		                 fs::file_size(manifest_models_dir / kPinnedModelArtifacts[1].filename,
		                               sface_size_ec) == kPinnedModelArtifacts[1].size &&
		                 !sface_size_ec,
		             "each official download matches its independently pinned artifact");

		const auto &official_yunet = howdy::native::kOfficialOpenCvModels[0];
		const auto &official_sface = howdy::native::kOfficialOpenCvModels[1];
		ok &= expect(official_yunet.type == kPinnedModelArtifacts[0].type &&
		                 official_yunet.filename == kPinnedModelArtifacts[0].filename &&
		                 official_yunet.url == kPinnedModelArtifacts[0].url &&
		                 official_yunet.size == kPinnedModelArtifacts[0].size &&
		                 official_yunet.sha256 == kPinnedModelArtifacts[0].sha256,
		             "official YuNet descriptor matches independently pinned artifact");
		ok &= expect(official_sface.type == kPinnedModelArtifacts[1].type &&
		                 official_sface.filename == kPinnedModelArtifacts[1].filename &&
		                 official_sface.url == kPinnedModelArtifacts[1].url &&
		                 official_sface.size == kPinnedModelArtifacts[1].size &&
		                 official_sface.sha256 == kPinnedModelArtifacts[1].sha256,
		             "official SFace descriptor matches independently pinned artifact");
		ok &= expect(official_yunet.filename != official_sface.filename &&
		                 official_yunet.url != official_sface.url &&
		                 official_yunet.size != official_sface.size &&
		                 official_yunet.sha256 != official_sface.sha256,
		             "official model descriptors remain distinct");

		fs::remove_all(temp_root, ec);
		return ok;
	}

}  // namespace

auto main() -> int {
	const UmaskGuard umask_guard(0022);
	const std::array results = {test_download_entrypoints(), test_atomic_files(),
	                            test_model_downloads(), test_official_manifest()};
	const bool       ok      = std::ranges::all_of(results, [](bool value) -> bool {
		return value;
	});
	return ok ? 0 : 1;
}
