#include "cli/download_models_cli.hpp"
#include "cli/download_models_internal.hpp"
#include "common/atomic_files.hpp"
#include "common/fd_io.hpp"
#include "common/file_security.hpp"
#include "common/model_file.hpp"
#include "config/runtime_paths.hpp"

#include <array>
#include <cerrno>
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
#include <system_error>
#include <unistd.h>
#include <utility>

#include <sys/stat.h>

#include <curl/curl.h>
#include <openssl/evp.h>

namespace {

	constexpr int  kExitOk                 = 0;
	constexpr int  kExitAbort              = EXIT_FAILURE;
	constexpr long kConnectTimeoutSeconds  = 15;
	constexpr long kTransferTimeoutSeconds = 300;
	constexpr long kLowSpeedBytesPerSecond = 1024;
	constexpr long kLowSpeedTimeoutSeconds = 30;

	using howdy::native::download_models_internal::StagedDownloadFile;

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

	auto configure_transfer_policy(CURL *curl) -> bool {
		bool ok =
		    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L) == CURLE_OK &&
		    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
		    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSeconds) == CURLE_OK &&
		    curl_easy_setopt(curl, CURLOPT_TIMEOUT, kTransferTimeoutSeconds) == CURLE_OK &&
		    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, kLowSpeedBytesPerSecond) == CURLE_OK &&
		    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, kLowSpeedTimeoutSeconds) == CURLE_OK &&
		    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE,
		                     static_cast<curl_off_t>(
		                         howdy::native::download_models_internal::kMaxDownloadBytes)) ==
		        CURLE_OK;
#ifdef CURLOPT_PROTOCOLS_STR
		ok = ok && curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https") == CURLE_OK;
#endif
#ifdef CURLOPT_REDIR_PROTOCOLS_STR
		ok = ok && curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https") == CURLE_OK;
#endif
		return ok;
	}

}  // namespace

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

	auto prepare_staged_download(const std::filesystem::path &destination,
	                             const std::optional<uid_t>   owner_uid)
	    -> std::optional<StagedDownloadFile> {
		const auto      parent = destination.parent_path();
		std::error_code ec;
		std::filesystem::create_directories(parent, ec);
		if (ec) {
			return std::nullopt;
		}

		if (std::filesystem::exists(parent)) {
			const auto dir_security = howdy::native::check_secure_root_owned_directory_tree(
			    parent, "Models directory", owner_uid);
			if (!dir_security.ok) {
				return std::nullopt;
			}
		}

		return howdy::native::prepare_staged_file(destination, ".howdy-download-");
	}

	auto download_file(const std::string &url, StagedDownloadFile &staged) -> bool {
		CURL *curl = curl_easy_init();
		if (curl == nullptr) {
			return false;
		}

		if (curl_easy_setopt(curl, CURLOPT_URL, url.c_str()) != CURLE_OK ||
		    !configure_transfer_policy(curl)) {
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

}  // namespace

auto howdy::native::download_models_internal::download_models_main_with_dependencies(
    int argc, char **argv, const DownloadModelsDependencies &dependencies) -> int {
	if (dependencies.download_file == nullptr || dependencies.model_file_owner_uid == nullptr ||
	    dependencies.sha256_file == nullptr || dependencies.fstat_file == nullptr) {
		return kExitAbort;
	}

	(void)argc;
	(void)argv;
	const auto      models_dir = howdy::native::resolve_models_dir();
	std::error_code models_dir_ec;
	std::filesystem::create_directories(models_dir, models_dir_ec);
	if (models_dir_ec) {
		std::cout << "Failed to create models directory: " << models_dir.string() << " ("
		          << models_dir_ec.message() << ")\n";
		return kExitAbort;
	}
	const auto owner_uid           = dependencies.model_file_owner_uid();
	const auto models_dir_security = howdy::native::check_secure_root_owned_directory_tree(
	    models_dir, "Models directory", owner_uid);
	if (!models_dir_security.ok) {
		std::cout << models_dir_security.error_message << "\n";
		return kExitAbort;
	}

	if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
		std::cout << "Failed to initialize download backend\n";
		return kExitAbort;
	}
	for (const auto &model : dependencies.models) {
		const auto destination = models_dir / model.filename;
		const auto readiness   = howdy::native::check_opencv_model_readiness_with_label(
		    destination, "Model file", owner_uid);
		if (readiness.status == howdy::native::OpenCvModelStatus::kInsecure) {
			curl_global_cleanup();
			std::cout << readiness.error_message << "\n";
			return kExitAbort;
		}

		bool replace_existing = readiness.status == howdy::native::OpenCvModelStatus::kInvalid;
		if (readiness.status == howdy::native::OpenCvModelStatus::kOk) {
			const int existing_fd = open(destination.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
			if (existing_fd < 0) {
				curl_global_cleanup();
				std::cout << "Failed to open existing model: " << destination.string() << "\n";
				return kExitAbort;
			}
			struct stat existing_stat{};
			if (dependencies.fstat_file(existing_fd, &existing_stat) != 0) {
				const int error_number = errno;
				close(existing_fd);
				curl_global_cleanup();
				std::cout << "Failed to fstat existing model '" << destination.string()
				          << "': " << std::strerror(error_number) << "\n";
				return kExitAbort;
			}
			if (existing_stat.st_size < 0 ||
			    std::cmp_not_equal(existing_stat.st_size, model.size)) {
				std::cout << "Size mismatch for " << destination.string() << ": expected "
				          << model.size << ", actual " << existing_stat.st_size << "\n";
				replace_existing = true;
			} else {
				const auto actual_sha256 = dependencies.sha256_file(existing_fd);
				if (!actual_sha256.has_value()) {
					close(existing_fd);
					curl_global_cleanup();
					std::cout << "Failed to calculate SHA-256 for " << model.filename << "\n";
					return kExitAbort;
				}
				replace_existing = model.sha256 != actual_sha256.value();
			}
			close(existing_fd);
			if (!replace_existing) {
				std::cout << "Model already exists: " << destination.string() << "\n";
				continue;
			}
		}
		if (replace_existing) {
			std::cout << "Replacing invalid model download: " << destination.string() << "\n";
		}

		std::cout << "Downloading " << model.filename << "\n";
		auto staged = prepare_staged_download(destination, owner_uid);
		if (!staged.has_value()) {
			curl_global_cleanup();
			std::cout << "Failed to prepare destination for model: " << destination.string()
			          << "\n";
			return kExitAbort;
		}

		if (!dependencies.download_file(std::string(model.url), *staged)) {
			howdy::native::cleanup_staged_file(*staged);
			curl_global_cleanup();
			std::cout << "Failed to download model: " << model.url << "\n";
			return kExitAbort;
		}

		struct stat staged_stat{};
		if (dependencies.fstat_file(staged->fd.get(), &staged_stat) != 0) {
			const int  error_number = errno;
			const auto staged_path  = staged->path;
			howdy::native::cleanup_staged_file(*staged);
			curl_global_cleanup();
			std::cout << "Failed to fstat staged model '" << staged_path.string()
			          << "': " << std::strerror(error_number) << "\n";
			return kExitAbort;
		}
		if (staged_stat.st_size < 0 || std::cmp_not_equal(staged_stat.st_size, model.size)) {
			howdy::native::cleanup_staged_file(*staged);
			curl_global_cleanup();
			std::cout << "Size mismatch for " << model.filename << ": expected " << model.size
			          << ", actual " << staged_stat.st_size << "\n";
			return kExitAbort;
		}

		const auto actual_sha256 = dependencies.sha256_file(staged->fd.get());
		if (!actual_sha256.has_value()) {
			howdy::native::cleanup_staged_file(*staged);
			curl_global_cleanup();
			std::cout << "Failed to calculate SHA-256 for " << model.filename << "\n";
			return kExitAbort;
		}
		if (model.sha256 != actual_sha256.value()) {
			howdy::native::cleanup_staged_file(*staged);
			curl_global_cleanup();
			std::cout << "Checksum mismatch for " << model.filename << "\n";
			return kExitAbort;
		}

		const auto install_result = howdy::native::install_staged_file(*staged, destination);
		if (!howdy::native::atomic_file_commit_is_durable(install_result)) {
			curl_global_cleanup();
			if (howdy::native::atomic_file_may_have_committed(install_result)) {
				std::cout
				    << "Downloaded model was installed, but its directory could not be synced: "
				    << destination.string() << "\n";
			} else {
				std::cout << "Failed to install downloaded model: " << destination.string() << "\n";
			}
			return kExitAbort;
		}
	}
	curl_global_cleanup();

	std::cout << "OpenCV face models ready in: " << models_dir.string() << "\n";
	return kExitOk;
}

int download_models_main(int argc, char **argv) {
	return howdy::native::download_models_internal::download_models_main_with_dependencies(
	    argc, argv,
	    howdy::native::download_models_internal::DownloadModelsDependencies{
	        .download_file        = download_file,
	        .model_file_owner_uid = root_model_file_owner_uid,
	    });
}
