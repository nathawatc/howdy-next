#include "cli/download_models.hpp"

#include "cli/download_models/internal.hpp"
#include "config/runtime_paths.hpp"
#include "model_assets/model_file.hpp"
#include "support/atomic_files.hpp"
#include "support/fd_io.hpp"
#include "support/file_security.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>

#include <sys/stat.h>

#include <curl/curl.h>
#include <openssl/evp.h>

namespace {

	constexpr int  kDownloadModelsExitOk    = 0;
	constexpr int  kDownloadModelsExitAbort = EXIT_FAILURE;
	constexpr long kConnectTimeoutSeconds   = 15;
	constexpr long kTransferTimeoutSeconds  = 300;
	constexpr long kLowSpeedBytesPerSecond  = 1024;
	constexpr long kLowSpeedTimeoutSeconds  = 30;

	using howdy::native::ScopedFd;
	using howdy::native::download_models_internal::CurlSetoptOperations;
	using howdy::native::download_models_internal::StagedDownloadFile;

	auto curl_setopt_long(void * /*context*/, CURL *curl, CURLoption option, const long value)
	    -> CURLcode {
		return curl_easy_setopt(curl, option, value);
	}

	auto curl_setopt_off_t(void * /*context*/, CURL *curl, CURLoption option,
	                       const curl_off_t value) -> CURLcode {
		return curl_easy_setopt(curl, option, value);
	}

	auto curl_setopt_string(void * /*context*/, CURL *curl, CURLoption option, const char *value)
	    -> CURLcode {
		return curl_easy_setopt(curl, option, value);
	}

	constexpr CurlSetoptOperations kCurlSetoptOperations{
	    .set_long   = curl_setopt_long,
	    .set_off_t  = curl_setopt_off_t,
	    .set_string = curl_setopt_string,
	};

	auto root_model_file_owner_uid() -> std::optional<uid_t> {
		return static_cast<uid_t>(0);
	}

	auto calculate_sha256_file_descriptor(const int fd) -> std::optional<std::string> {
		if (lseek(fd, 0, SEEK_SET) < 0) {
			return std::nullopt;
		}

		EVP_MD_CTX *context = EVP_MD_CTX_new();
		if (context == nullptr) {
			return std::nullopt;
		}
		if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
			EVP_MD_CTX_free(context);
			return std::nullopt;
		}

		std::array<unsigned char, 8192> buffer{};
		while (true) {
			const auto bytes_read = read(fd, buffer.data(), buffer.size());
			if (bytes_read == 0) {
				break;
			}
			if (bytes_read < 0) {
				if (errno == EINTR) {
					continue;
				}
				EVP_MD_CTX_free(context);
				return std::nullopt;
			}
			if (EVP_DigestUpdate(context, buffer.data(), static_cast<std::size_t>(bytes_read)) !=
			    1) {
				EVP_MD_CTX_free(context);
				return std::nullopt;
			}
		}

		std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
		unsigned int                               digest_length = 0;
		const bool                                 digest_ok =
		    EVP_DigestFinal_ex(context, digest.data(), &digest_length) == 1 && digest_length == 32U;
		EVP_MD_CTX_free(context);
		if (!digest_ok) {
			return std::nullopt;
		}

		std::ostringstream output;
		output << std::hex << std::setfill('0');
		for (unsigned int index = 0; index < digest_length; ++index) {
			output << std::setw(2) << static_cast<unsigned int>(digest[index]);
		}
		return output.str();
	}

}  // namespace

auto howdy::native::download_models_internal::configure_transfer_policy(
    CURL *curl, const CurlSetoptOperations &operations) -> bool {
	if (operations.set_long == nullptr || operations.set_off_t == nullptr ||
	    operations.set_string == nullptr) {
		return false;
	}

	return operations.set_long(operations.context, curl, CURLOPT_FOLLOWLOCATION, 1L) == CURLE_OK &&
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

auto howdy::native::download_models_internal::sha256_file_descriptor(const int fd)
    -> std::optional<std::string> {
	return calculate_sha256_file_descriptor(fd);
}

auto howdy::native::download_models_internal::download_models_write_callback(
    void *contents, size_t size, size_t nmemb, void *userp) -> size_t {
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

	if (!howdy::native::write_all_to_fd(context->staged->fd.get(), data, total)) {
		return 0;
	}
	context->bytes_written += total;
	return total;
}

namespace {

	auto models_directory_failure(std::string_view operation, const std::filesystem::path &path,
	                              const int error_number) -> howdy::native::SecurePathCheckResult {
		return {.ok            = false,
		        .error_message = "Failed to " + std::string(operation) + " " +
		                         std::string(howdy::native::kModelsDirectoryLabel) + ": " +
		                         path.string() + " (" + std::strerror(error_number) + ")",
		        .error_code    = error_number};
	}

	auto validate_models_directory_component(const int fd, const std::filesystem::path &path,
	                                         const std::optional<uid_t> owner_uid)
	    -> howdy::native::SecurePathCheckResult {
		struct stat component_stat{};
		if (fstat(fd, &component_stat) != 0) {
			return models_directory_failure("inspect", path, errno);
		}
		return howdy::native::check_secure_path_stat(
		    component_stat, howdy::native::SecurePathKind::kDirectory, path,
		    howdy::native::kModelsDirectoryLabel, owner_uid);
	}

	auto create_secure_models_directory(const std::filesystem::path &models_dir,
	                                    const std::optional<uid_t>   owner_uid)
	    -> howdy::native::SecurePathCheckResult {
		if (!models_dir.is_absolute()) {
			return models_directory_failure("open", models_dir, EINVAL);
		}

		constexpr int kDirectoryOpenFlags   = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
		const auto    normalized_models_dir = models_dir.lexically_normal();
		const auto    root                  = normalized_models_dir.root_path();
		ScopedFd      current(open(root.c_str(), kDirectoryOpenFlags));
		if (current.get() < 0) {
			return models_directory_failure("open", root, errno);
		}

		auto current_path = root;
		if (auto security =
		        validate_models_directory_component(current.get(), current_path, owner_uid);
		    !security.ok) {
			return security;
		}

		for (const auto &component : normalized_models_dir.relative_path()) {
			if (component == std::filesystem::path(".")) {
				continue;
			}

			const auto component_path = current_path / component;
			ScopedFd   next(openat(current.get(), component.c_str(), kDirectoryOpenFlags));
			if (next.get() < 0) {
				const int open_error = errno;
				if (open_error != ENOENT) {
					const auto security = howdy::native::check_secure_root_owned_directory_tree(
					    component_path, howdy::native::kModelsDirectoryLabel, owner_uid);
					return security.ok
					           ? models_directory_failure("open", component_path, open_error)
					           : security;
				}
				if (mkdirat(current.get(), component.c_str(), 0755) != 0 && errno != EEXIST) {
					return models_directory_failure("create", component_path, errno);
				}
				next.reset(openat(current.get(), component.c_str(), kDirectoryOpenFlags));
				if (next.get() < 0) {
					return models_directory_failure("open", component_path, errno);
				}
			}

			if (auto security =
			        validate_models_directory_component(next.get(), component_path, owner_uid);
			    !security.ok) {
				return security;
			}
			current      = std::move(next);
			current_path = component_path;
		}

		return howdy::native::check_secure_root_owned_directory_tree(
		    normalized_models_dir, howdy::native::kModelsDirectoryLabel, owner_uid);
	}

	auto prepare_staged_download(const std::filesystem::path &destination,
	                             const std::optional<uid_t>   owner_uid)
	    -> std::optional<StagedDownloadFile> {
		const auto parent       = destination.parent_path();
		const auto dir_security = howdy::native::check_secure_root_owned_directory_tree(
		    parent, howdy::native::kModelsDirectoryLabel, owner_uid);
		if (!dir_security.ok) {
			return std::nullopt;
		}

		return howdy::native::prepare_staged_file(
		    destination, ".howdy-download-", howdy::native::kDefaultAtomicFileMode,
		    howdy::native::StagedFileMetadataPolicy::kPreserveExisting,
		    howdy::native::StagedFileParentPolicy::kRequireExisting);
	}

	auto download_file(const std::string &url, StagedDownloadFile &staged) -> bool {
		CURL *curl = curl_easy_init();
		if (curl == nullptr) {
			return false;
		}

		if (curl_easy_setopt(curl, CURLOPT_URL, url.c_str()) != CURLE_OK ||
		    !howdy::native::download_models_internal::configure_transfer_policy(
		        curl, kCurlSetoptOperations)) {
			curl_easy_cleanup(curl);
			return false;
		}
		howdy::native::download_models_internal::DownloadWriteContext write_context{
		    .staged = &staged,
		};
		if (curl_easy_setopt(
		        curl, CURLOPT_WRITEFUNCTION,
		        howdy::native::download_models_internal::download_models_write_callback) !=
		        CURLE_OK ||
		    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &write_context) != CURLE_OK) {
			curl_easy_cleanup(curl);
			return false;
		}
		const CURLcode result = curl_easy_perform(curl);

		long           status_code = 0;
		const CURLcode info_result = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
		curl_easy_cleanup(curl);
		return result == CURLE_OK && info_result == CURLE_OK && status_code >= 200 &&
		       status_code < 400;
	}

	enum class ExistingModelAction : std::uint8_t {
		download,
		skip,
		abort,
	};

	auto inspect_existing_model(
	    const std::filesystem::path &destination, const howdy::native::OpenCvModelDescriptor &model,
	    const howdy::native::download_models_internal::DownloadModelsDependencies &dependencies)
	    -> ExistingModelAction {
		const int existing_fd = open(destination.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		if (existing_fd < 0) {
			std::cout << "Failed to open existing model: " << destination.string() << "\n";
			return ExistingModelAction::abort;
		}
		struct stat existing_stat{};
		if (dependencies.fstat_file(existing_fd, &existing_stat) != 0) {
			const int error_number = errno;
			close(existing_fd);
			std::cout << "Failed to fstat existing model '" << destination.string()
			          << "': " << std::strerror(error_number) << "\n";
			return ExistingModelAction::abort;
		}
		if (existing_stat.st_size < 0 || std::cmp_not_equal(existing_stat.st_size, model.size)) {
			close(existing_fd);
			std::cout << "Size mismatch for " << destination.string() << ": expected " << model.size
			          << ", actual " << existing_stat.st_size << "\n";
			return ExistingModelAction::download;
		}
		const auto actual_sha256 = dependencies.sha256_file(existing_fd);
		close(existing_fd);
		if (!actual_sha256.has_value()) {
			std::cout << "Failed to calculate SHA-256 for " << model.filename << "\n";
			return ExistingModelAction::abort;
		}
		return model.sha256 == *actual_sha256 ? ExistingModelAction::skip
		                                      : ExistingModelAction::download;
	}

	auto download_model(
	    const howdy::native::OpenCvModelDescriptor &model, const std::filesystem::path &destination,
	    const std::optional<uid_t>                                                 owner_uid,
	    const howdy::native::download_models_internal::DownloadModelsDependencies &dependencies)
	    -> bool {
		std::cout << "Downloading " << model.filename << "\n";
		auto staged = prepare_staged_download(destination, owner_uid);
		if (!staged.has_value()) {
			std::cout << "Failed to prepare destination for model: " << destination.string()
			          << "\n";
			return false;
		}
		if (!dependencies.download_file(std::string(model.url), *staged)) {
			howdy::native::cleanup_staged_file(*staged);
			std::cout << "Failed to download model: " << model.url << "\n";
			return false;
		}

		struct stat staged_stat{};
		if (dependencies.fstat_file(staged->fd.get(), &staged_stat) != 0) {
			const int  error_number = errno;
			const auto staged_path  = staged->path;
			howdy::native::cleanup_staged_file(*staged);
			std::cout << "Failed to fstat staged model '" << staged_path.string()
			          << "': " << std::strerror(error_number) << "\n";
			return false;
		}
		if (staged_stat.st_size < 0 || std::cmp_not_equal(staged_stat.st_size, model.size)) {
			howdy::native::cleanup_staged_file(*staged);
			std::cout << "Size mismatch for " << model.filename << ": expected " << model.size
			          << ", actual " << staged_stat.st_size << "\n";
			return false;
		}

		const auto actual_sha256 = dependencies.sha256_file(staged->fd.get());
		if (!actual_sha256.has_value() || model.sha256 != *actual_sha256) {
			howdy::native::cleanup_staged_file(*staged);
			std::cout << (actual_sha256.has_value() ? "Checksum mismatch for "
			                                        : "Failed to calculate SHA-256 for ")
			          << model.filename << "\n";
			return false;
		}

		const auto install_result = howdy::native::install_staged_file(*staged, destination);
		if (howdy::native::atomic_file_commit_is_durable(install_result)) {
			return true;
		}
		if (howdy::native::atomic_file_may_have_committed(install_result)) {
			std::cout << "Downloaded model was installed, but its directory could not be synced: "
			          << destination.string() << "\n";
		} else {
			std::cout << "Failed to install downloaded model: " << destination.string() << "\n";
		}
		return false;
	}

}  // namespace

auto howdy::native::download_models_internal::download_models_main_with_dependencies(
    int argc, char **argv, const DownloadModelsDependencies &dependencies) -> int {
	if (argc != 1) {
		std::cout << "Invalid arguments for download-models\n";
		return kDownloadModelsExitAbort;
	}
	(void)argv;
	if (dependencies.download_file == nullptr || dependencies.model_file_owner_uid == nullptr ||
	    dependencies.sha256_file == nullptr || dependencies.fstat_file == nullptr) {
		return kDownloadModelsExitAbort;
	}

	const auto models_dir          = howdy::native::resolve_models_dir();
	const auto owner_uid           = dependencies.model_file_owner_uid();
	const auto models_dir_security = create_secure_models_directory(models_dir, owner_uid);
	if (!models_dir_security.ok) {
		std::cout << "Failed to create models directory: " << models_dir_security.error_message
		          << "\n";
		return kDownloadModelsExitAbort;
	}

	if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
		std::cout << "Failed to initialize download backend\n";
		return kDownloadModelsExitAbort;
	}
	for (const auto &model : dependencies.models) {
		const auto destination = models_dir / model.filename;
		const auto readiness   = howdy::native::check_opencv_model_readiness_with_label(
		    destination, "Model file", owner_uid);
		if (readiness.status == howdy::native::OpenCvModelStatus::kInsecure) {
			curl_global_cleanup();
			std::cout << readiness.error_message << "\n";
			return kDownloadModelsExitAbort;
		}

		bool replace_existing = readiness.status == howdy::native::OpenCvModelStatus::kInvalid;
		if (readiness.status == howdy::native::OpenCvModelStatus::kOk) {
			const auto action = inspect_existing_model(destination, model, dependencies);
			if (action == ExistingModelAction::abort) {
				curl_global_cleanup();
				return kDownloadModelsExitAbort;
			}
			replace_existing = action == ExistingModelAction::download;
			if (action == ExistingModelAction::skip) {
				std::cout << "Model already exists: " << destination.string() << "\n";
				continue;
			}
		}
		if (replace_existing) {
			std::cout << "Replacing invalid model download: " << destination.string() << "\n";
		}
		if (!download_model(model, destination, owner_uid, dependencies)) {
			curl_global_cleanup();
			return kDownloadModelsExitAbort;
		}
	}
	curl_global_cleanup();

	std::cout << "OpenCV face models ready in: " << models_dir.string() << "\n";
	return kDownloadModelsExitOk;
}

auto download_models_main(int argc, char **argv) -> int {
	return howdy::native::download_models_internal::download_models_main_with_dependencies(
	    argc, argv,
	    howdy::native::download_models_internal::DownloadModelsDependencies{
	        .download_file        = download_file,
	        .model_file_owner_uid = root_model_file_owner_uid,
	    });
}
