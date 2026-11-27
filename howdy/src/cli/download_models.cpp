#include "cli/download_models.hpp"

#include "app/command_invocation.hpp"
#include "cli/download_models/internal.hpp"
#include "config/runtime_paths.hpp"
#include "model_assets/model_file.hpp"
#include "model_assets/opencv_model_manifest.hpp"
#include "support/atomic_files.hpp"
#include "support/eintr.hpp"
#include "support/error_message.hpp"
#include "support/fd_io.hpp"
#include "support/file_security.hpp"
#include "support/file_security/validation_root.hpp"
#include "support/scoped_fd.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

#include <sys/types.h>

#include <curl/curl.h>
#include <curl/easy.h>
#include <curl/system.h>
#include <openssl/evp.h>

namespace {

	using howdy::native::download_models_internal::CurlLong;

	constexpr std::string_view kModelFileLabel          = "Model file";
	constexpr int              kDownloadModelsExitOk    = 0;
	constexpr int              kDownloadModelsExitAbort = EXIT_FAILURE;
	constexpr CurlLong         kConnectTimeoutSeconds   = 15;
	constexpr CurlLong         kTransferTimeoutSeconds  = 300;
	constexpr CurlLong         kLowSpeedBytesPerSecond  = 1024;
	constexpr CurlLong         kLowSpeedTimeoutSeconds  = 30;
	constexpr CurlLong         kMaxRedirects            = 5;

	using howdy::native::ScopedFd;
	using howdy::native::download_models_internal::CurlSetoptOperations;
	using howdy::native::download_models_internal::StagedDownloadFile;

	auto CurlSetoptLong(void * /*context*/, CURL *curl, CURLoption option, const CurlLong value)
	    -> CURLcode {
		return curl_easy_setopt(curl, option, value);
	}

	auto CurlSetoptOffT(void * /*context*/, CURL *curl, CURLoption option, const curl_off_t value)
	    -> CURLcode {
		return curl_easy_setopt(curl, option, value);
	}

	auto CurlSetoptString(void * /*context*/, CURL *curl, CURLoption option, const char *value)
	    -> CURLcode {
		return curl_easy_setopt(curl, option, value);
	}

	constexpr CurlSetoptOperations kCurlSetoptOperations{
	    .set_long   = CurlSetoptLong,
	    .set_off_t  = CurlSetoptOffT,
	    .set_string = CurlSetoptString,
	};

	auto RootModelFileOwnerUid() -> std::optional<uid_t> {
		return static_cast<uid_t>(0);
	}

	struct EvpMdCtxDeleter {
		void operator()(EVP_MD_CTX *ctx) const noexcept {
			EVP_MD_CTX_free(ctx);
		}
	};

	using ScopedEvpMdCtx = std::unique_ptr<EVP_MD_CTX, EvpMdCtxDeleter>;

	struct CurlEasyDeleter {
		void operator()(CURL *curl) const noexcept {
			curl_easy_cleanup(curl);
		}
	};

	using ScopedCurlEasy = std::unique_ptr<CURL, CurlEasyDeleter>;

	class CurlGlobalGuard {
	public:
		CurlGlobalGuard() noexcept
		    : ok_(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK) {}

		~CurlGlobalGuard() {
			if (ok_) {
				curl_global_cleanup();
			}
		}

		CurlGlobalGuard(const CurlGlobalGuard &)                     = delete;
		auto operator=(const CurlGlobalGuard &) -> CurlGlobalGuard & = delete;
		CurlGlobalGuard(CurlGlobalGuard &&)                          = delete;
		auto operator=(CurlGlobalGuard &&) -> CurlGlobalGuard &      = delete;

		[[nodiscard]] auto Ok() const noexcept -> bool {
			return ok_;
		}

	private:
		bool ok_ = false;
	};

}  // namespace

auto howdy::native::download_models_internal::ConfigureTransferPolicy(
    CURL *curl, const CurlSetoptOperations &operations) -> bool {
	if (operations.set_long == nullptr || operations.set_off_t == nullptr ||
	    operations.set_string == nullptr) {
		return false;
	}

	return operations.set_long(operations.context, curl, CURLOPT_FOLLOWLOCATION, 1L) == CURLE_OK &&
	       operations.set_long(operations.context, curl, CURLOPT_MAXREDIRS, kMaxRedirects) ==
	           CURLE_OK &&
	       operations.set_long(operations.context, curl, CURLOPT_SSL_VERIFYPEER, 1L) == CURLE_OK &&
	       operations.set_long(operations.context, curl, CURLOPT_SSL_VERIFYHOST, 2L) == CURLE_OK &&
	       operations.set_long(operations.context, curl, CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
	       operations.set_long(operations.context, curl, CURLOPT_CONNECTTIMEOUT,
	                           kConnectTimeoutSeconds) == CURLE_OK &&
	       operations.set_long(operations.context, curl, CURLOPT_TIMEOUT,
	                           kTransferTimeoutSeconds) == CURLE_OK &&
	       operations.set_long(operations.context, curl, CURLOPT_LOW_SPEED_LIMIT,
	                           kLowSpeedBytesPerSecond) == CURLE_OK &&
	       operations.set_long(operations.context, curl, CURLOPT_LOW_SPEED_TIME,
	                           kLowSpeedTimeoutSeconds) == CURLE_OK &&
	       operations.set_off_t(operations.context, curl, CURLOPT_MAXFILESIZE_LARGE,
	                            static_cast<curl_off_t>(kMaxDownloadBytes)) == CURLE_OK &&
	       operations.set_string(operations.context, curl, CURLOPT_PROTOCOLS_STR, "https") ==
	           CURLE_OK &&
	       operations.set_string(operations.context, curl, CURLOPT_REDIR_PROTOCOLS_STR, "https") ==
	           CURLE_OK;
}

auto howdy::native::download_models_internal::Sha256FileDescriptor(const int fd)
    -> std::optional<std::string> {
	if (lseek(fd, 0, SEEK_SET) < 0) {
		return std::nullopt;
	}

	const ScopedEvpMdCtx context(EVP_MD_CTX_new());
	if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
		return std::nullopt;
	}

	std::array<unsigned char, 8192> buffer{};
	while (true) {
		const auto bytes_read = howdy::native::RetryOnEintr([&] -> ssize_t {
			return read(fd, buffer.data(), buffer.size());
		});
		if (bytes_read == 0) {
			break;
		}
		if (bytes_read < 0) {
			return std::nullopt;
		}
		if (EVP_DigestUpdate(context.get(), buffer.data(), static_cast<std::size_t>(bytes_read)) !=
		    1) {
			return std::nullopt;
		}
	}

	std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
	unsigned int                               digest_length = 0;
	if (EVP_DigestFinal_ex(context.get(), digest.data(), &digest_length) != 1 ||
	    digest_length != 32U) {
		return std::nullopt;
	}

	std::string hex;
	hex.reserve(static_cast<std::size_t>(digest_length) * 2);
	for (const unsigned char byte : std::span(digest).first(digest_length)) {
		std::format_to(std::back_inserter(hex), "{:02x}", byte);
	}
	return hex;
}

auto howdy::native::download_models_internal::DownloadModelsWriteCallback(void  *contents,
                                                                          size_t size, size_t nmemb,
                                                                          void *userp) -> size_t {
	auto *context =
	    static_cast<howdy::native::download_models_internal::DownloadWriteContext *>(userp);
	if (context == nullptr || context->staged == nullptr) {
		return 0;
	}
	const auto *data = static_cast<const char *>(contents);
	if (size != 0 && nmemb > std::numeric_limits<std::size_t>::max() / size) {
		return 0;
	}
	const auto total = size * nmemb;
	if (context->bytes_written > context->max_bytes ||
	    total > context->max_bytes - context->bytes_written) {
		return 0;
	}

	if (!howdy::native::WriteAllToFd(context->staged->Fd().Get(), data, total)) {
		return 0;
	}
	context->bytes_written += total;
	return total;
}

namespace {

	auto PrepareStagedDownload(
	    const std::filesystem::path &destination, const std::optional<uid_t> owner_uid,
	    const howdy::native::file_security_internal::ValidationRoot &validation_root)
	    -> std::optional<StagedDownloadFile> {
		// The same parent-plus-full-path check model readiness uses, so the writer never stages a
		// path that readiness would reject.
		const auto dir_security = howdy::native::CheckSecureRootOwnedFileParent(
		    destination,
		    {.directory = howdy::native::kModelsDirectoryLabel, .file = kModelFileLabel}, owner_uid,
		    validation_root);
		if (!dir_security.ok) {
			std::cout << dir_security.error_message << "\n";
			return std::nullopt;
		}

		return howdy::native::PrepareStagedFile(
		    destination, ".howdy-download-", howdy::native::kDefaultAtomicFileMode,
		    howdy::native::StagedFileMetadataPolicy::kPreserveExisting,
		    howdy::native::StagedFileParentPolicy::kRequireExisting);
	}

	auto DownloadFile(const std::string &url, StagedDownloadFile &staged) -> bool {
		const ScopedCurlEasy curl(curl_easy_init());
		if (!curl) {
			return false;
		}

		if (curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str()) != CURLE_OK ||
		    !howdy::native::download_models_internal::ConfigureTransferPolicy(
		        curl.get(), kCurlSetoptOperations)) {
			return false;
		}
		howdy::native::download_models_internal::DownloadWriteContext write_context{
		    .staged = &staged,
		};
		if (curl_easy_setopt(
		        curl.get(), CURLOPT_WRITEFUNCTION,
		        howdy::native::download_models_internal::DownloadModelsWriteCallback) != CURLE_OK ||
		    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &write_context) != CURLE_OK) {
			return false;
		}
		const CURLcode result = curl_easy_perform(curl.get());

		CurlLong       status_code = 0;
		const CURLcode info_result =
		    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status_code);
		return result == CURLE_OK && info_result == CURLE_OK && status_code >= 200 &&
		       status_code < 400;
	}

	auto DownloadModel(
	    const howdy::native::OpenCvModelDescriptor &model, const std::filesystem::path &destination,
	    const std::optional<uid_t>                                                 owner_uid,
	    const howdy::native::download_models_internal::DownloadModelsDependencies &dependencies)
	    -> bool {
		std::cout << "Downloading " << model.filename << "\n";
		auto staged = PrepareStagedDownload(destination, owner_uid, dependencies.validation_root);
		if (!staged.has_value()) {
			std::cout << "Failed to prepare destination for model: " << destination.string()
			          << "\n";
			return false;
		}
		if (!dependencies.download_file(std::string(model.url), *staged)) {
			std::cout << "Failed to download model: " << model.url << "\n";
			return false;
		}

		struct stat staged_stat{};
		if (dependencies.fstat_file(staged->Fd().Get(), &staged_stat) != 0) {
			const int  error_number = errno;
			const auto staged_path  = staged->Path();
			std::cout << "Failed to fstat staged model '" << staged_path.string()
			          << "': " << howdy::native::SystemErrorMessage(error_number) << "\n";
			return false;
		}
		if (staged_stat.st_size < 0 || std::cmp_not_equal(staged_stat.st_size, model.size)) {
			std::cout << "Size mismatch for " << model.filename << ": expected " << model.size
			          << ", actual " << staged_stat.st_size << "\n";
			return false;
		}

		const auto actual_sha256 = dependencies.sha256_file(staged->Fd().Get());
		if (!actual_sha256.has_value() || model.sha256 != *actual_sha256) {
			std::cout << (actual_sha256.has_value() ? "Checksum mismatch for "
			                                        : "Failed to calculate SHA-256 for ")
			          << model.filename << "\n";
			return false;
		}

		const auto install_result = howdy::native::InstallStagedFile(*staged, destination);
		if (howdy::native::AtomicFileCommitIsDurable(install_result)) {
			return true;
		}
		if (howdy::native::AtomicFileMayHaveCommitted(install_result)) {
			std::cout << "Downloaded model was installed, but its directory could not be synced: "
			          << destination.string() << "\n";
		} else {
			std::cout << "Failed to install downloaded model: " << destination.string() << "\n";
		}
		return false;
	}

}  // namespace

auto howdy::native::download_models_internal::InspectExistingModel(
    const std::filesystem::path &destination, const howdy::native::OpenCvModelDescriptor &model,
    const std::optional<uid_t> owner_uid, const DownloadModelsDependencies &dependencies)
    -> ExistingModelAction {
	if (dependencies.fstat_file == nullptr || dependencies.sha256_file == nullptr) {
		return ExistingModelAction::kAbort;
	}
	// O_NONBLOCK prevents blocking indefinitely if a FIFO replaces the regular file between
	// CheckOpencvModelReadiness() and open(); CheckSecurePathStat() rejects non-regular or
	// insecure replacements before hashing.
	const ScopedFd existing_fd(
	    open(destination.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
	if (!existing_fd.Valid()) {
		std::cout << "Failed to open existing model: " << destination.string() << "\n";
		return ExistingModelAction::kAbort;
	}
	struct stat existing_stat{};
	if (dependencies.fstat_file(existing_fd.Get(), &existing_stat) != 0) {
		const int error_number = errno;
		std::cout << "Failed to fstat existing model '" << destination.string()
		          << "': " << howdy::native::SystemErrorMessage(error_number) << "\n";
		return ExistingModelAction::kAbort;
	}
	if (const auto security = howdy::native::CheckSecurePathStat(
	        existing_stat, howdy::native::SecurePathKind::kRegularFile, destination,
	        kModelFileLabel, owner_uid);
	    !security.ok) {
		std::cout << security.error_message << "\n";
		return ExistingModelAction::kAbort;
	}
	if (existing_stat.st_size < 0 || std::cmp_not_equal(existing_stat.st_size, model.size)) {
		std::cout << "Size mismatch for " << destination.string() << ": expected " << model.size
		          << ", actual " << existing_stat.st_size << "\n";
		return ExistingModelAction::kDownload;
	}
	const auto actual_sha256 = dependencies.sha256_file(existing_fd.Get());
	if (!actual_sha256.has_value()) {
		std::cout << "Failed to calculate SHA-256 for " << model.filename << "\n";
		return ExistingModelAction::kAbort;
	}
	return model.sha256 == *actual_sha256 ? ExistingModelAction::kSkip
	                                      : ExistingModelAction::kDownload;
}

auto howdy::native::download_models_internal::DownloadModelsMainWithDependencies(
    [[maybe_unused]] const howdy::native::CommandInvocation &invocation,
    const DownloadModelsDependencies                        &dependencies) -> int {
	if (dependencies.download_file == nullptr || dependencies.model_file_owner_uid == nullptr ||
	    dependencies.sha256_file == nullptr || dependencies.fstat_file == nullptr) {
		return kDownloadModelsExitAbort;
	}

	const auto models_dir          = howdy::native::ResolveModelsDir();
	const auto owner_uid           = dependencies.model_file_owner_uid();
	const auto models_dir_security = howdy::native::CreateSecureDirectoryTree(
	    models_dir, howdy::native::kModelsDirectoryLabel, 0755, 0755, owner_uid,
	    dependencies.validation_root);
	if (!models_dir_security.ok) {
		std::cout << "Failed to create models directory: " << models_dir_security.error_message
		          << "\n";
		return kDownloadModelsExitAbort;
	}

	const CurlGlobalGuard curl_guard;
	if (!curl_guard.Ok()) {
		std::cout << "Failed to initialize download backend\n";
		return kDownloadModelsExitAbort;
	}
	for (const auto &model : dependencies.models) {
		const auto destination = models_dir / model.filename;
		const auto readiness   = howdy::native::CheckOpencvModelReadiness(
		    destination, kModelFileLabel, owner_uid, dependencies.validation_root);
		if (readiness.status == howdy::native::OpenCvModelStatus::kInsecure) {
			std::cout << readiness.error_message << "\n";
			return kDownloadModelsExitAbort;
		}

		bool replace_existing = readiness.status == howdy::native::OpenCvModelStatus::kInvalid;
		if (readiness.status == howdy::native::OpenCvModelStatus::kOk) {
			const auto action = InspectExistingModel(destination, model, owner_uid, dependencies);
			if (action == ExistingModelAction::kAbort) {
				return kDownloadModelsExitAbort;
			}
			replace_existing = action == ExistingModelAction::kDownload;
			if (action == ExistingModelAction::kSkip) {
				std::cout << "Model already exists: " << destination.string() << "\n";
				continue;
			}
		}
		if (replace_existing) {
			std::cout << "Replacing invalid model download: " << destination.string() << "\n";
		}
		if (!DownloadModel(model, destination, owner_uid, dependencies)) {
			return kDownloadModelsExitAbort;
		}
	}

	std::cout << "OpenCV face models ready in: " << models_dir.string() << "\n";
	return kDownloadModelsExitOk;
}

auto DownloadModelsMain(const howdy::native::CommandInvocation &invocation) -> int {
	return howdy::native::download_models_internal::DownloadModelsMainWithDependencies(
	    invocation, howdy::native::download_models_internal::DownloadModelsDependencies{
	                    .download_file        = DownloadFile,
	                    .model_file_owner_uid = RootModelFileOwnerUid,
	                });
}
