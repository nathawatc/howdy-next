#include "cli/download_models_cli.hpp"
#include "cli/download_models_internal.hpp"
#include "common/atomic_files.hpp"
#include "common/fd_io.hpp"
#include "common/file_security.hpp"
#include "common/model_file.hpp"
#include "config/runtime_paths.hpp"
#include "core/face_model.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>

#include <curl/curl.h>
#include <openssl/evp.h>

namespace {

	constexpr int  kExitOk                 = 0;
	constexpr int  kExitAbort              = EXIT_FAILURE;
	constexpr long kConnectTimeoutSeconds  = 15;
	constexpr long kTransferTimeoutSeconds = 300;
	constexpr long kLowSpeedBytesPerSecond = 1024;
	constexpr long kLowSpeedTimeoutSeconds = 30;

	struct ModelDownload {
		std::string           name;
		std::string           url;
		std::filesystem::path destination;
		std::string           sha256;
	};

	using howdy::native::download_models_internal::StagedDownloadFile;

	auto root_model_file_owner_uid() -> std::optional<uid_t> {
		return static_cast<uid_t>(0);
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

	auto file_sha256(const std::filesystem::path &path) -> std::optional<std::string> {
		std::ifstream input(path, std::ios::binary);
		if (!input.is_open()) {
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

		std::array<char, 8192> buffer{};
		while (input.good()) {
			input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
			const auto bytes_read = input.gcount();
			if (bytes_read > 0) {
				if (EVP_DigestUpdate(context, buffer.data(),
				                     static_cast<std::size_t>(bytes_read)) != 1) {
					EVP_MD_CTX_free(context);
					return std::nullopt;
				}
			}
		}
		if (input.bad()) {
			EVP_MD_CTX_free(context);
			return std::nullopt;
		}

		std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
		unsigned int                               digest_length = 0;
		if (EVP_DigestFinal_ex(context, digest.data(), &digest_length) != 1) {
			EVP_MD_CTX_free(context);
			return std::nullopt;
		}
		EVP_MD_CTX_free(context);
		if (digest_length != 32U) {
			return std::nullopt;
		}

		std::ostringstream out;
		out << std::hex << std::setfill('0');
		for (unsigned int index = 0; index < digest_length; ++index) {
			out << std::setw(2) << static_cast<unsigned int>(digest[index]);
		}
		return out.str();
	}

	auto prepare_staged_download(const std::filesystem::path &destination)
	    -> std::optional<StagedDownloadFile> {
		const auto      parent = destination.parent_path();
		std::error_code ec;
		std::filesystem::create_directories(parent, ec);
		if (ec) {
			return std::nullopt;
		}

		if (std::filesystem::exists(parent)) {
			const auto dir_security =
			    howdy::native::check_secure_root_owned_directory_tree(parent, "Models directory");
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
	if (dependencies.download_file == nullptr || dependencies.model_file_owner_uid == nullptr) {
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
	const auto models_dir_security =
	    howdy::native::check_secure_root_owned_directory_tree(models_dir, "Models directory");
	if (!models_dir_security.ok) {
		std::cout << models_dir_security.error_message << "\n";
		return kExitAbort;
	}

	const std::array<ModelDownload, 2> models = {
	    ModelDownload{
	        .name = howdy::native::FaceModel::kYunetModel,
	        .url =
	            "https://github.com/opencv/opencv_zoo/raw/26cc381e4d2594bb9f47a26eb8fd96c94a13660d/"
	            "models/face_detection_yunet/" +
	            std::string(howdy::native::FaceModel::kYunetModel),
	        .destination = models_dir / howdy::native::FaceModel::kYunetModel,
	        .sha256      = "ebafce4e3c118d6554634be5c27ab333b4c047a9a8c3faf1d7cf93101c22f0f0",
	    },
	    ModelDownload{
	        .name = howdy::native::FaceModel::kSfaceModel,
	        .url =
	            "https://github.com/opencv/opencv_zoo/raw/088c3571ec70df15100a5e4c26894d95951e92e9/"
	            "models/face_recognition_sface/" +
	            std::string(howdy::native::FaceModel::kSfaceModel),
	        .destination = models_dir / howdy::native::FaceModel::kSfaceModel,
	        .sha256      = "2b0e941e6f16cc048c20aee0c8e31f569118f65d702914540f7bfdc14048d78a",
	    },
	};

	if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
		std::cout << "Failed to initialize download backend\n";
		return kExitAbort;
	}
	for (const auto &model : models) {
		const auto readiness = howdy::native::check_opencv_model_readiness_with_label(
		    model.destination, "Model file", dependencies.model_file_owner_uid());
		if (readiness.status == howdy::native::OpenCvModelStatus::kInsecure) {
			curl_global_cleanup();
			std::cout << readiness.error_message << "\n";
			return kExitAbort;
		}
		if (readiness.status == howdy::native::OpenCvModelStatus::kOk) {
			std::cout << "Model already exists: " << model.destination.string() << "\n";
			continue;
		}
		if (readiness.status == howdy::native::OpenCvModelStatus::kInvalid) {
			std::cout << "Replacing invalid model download: " << model.destination.string() << "\n";
		}

		std::cout << "Downloading " << model.name << "\n";
		auto staged = prepare_staged_download(model.destination);
		if (!staged.has_value()) {
			curl_global_cleanup();
			std::cout << "Failed to prepare destination for model: " << model.destination.string()
			          << "\n";
			return kExitAbort;
		}

		if (!dependencies.download_file(model.url, *staged)) {
			howdy::native::cleanup_staged_file(*staged);
			curl_global_cleanup();
			std::cout << "Failed to download model: " << model.url << "\n";
			return kExitAbort;
		}

		if (howdy::native::is_invalid_model_file(staged->path)) {
			howdy::native::cleanup_staged_file(*staged);
			curl_global_cleanup();
			std::cout << "Downloaded file is not an ONNX model: " << model.url << "\n";
			return kExitAbort;
		}

		const auto actual_sha256 = file_sha256(staged->path);
		if (!actual_sha256.has_value() || model.sha256 != actual_sha256.value()) {
			howdy::native::cleanup_staged_file(*staged);
			curl_global_cleanup();
			std::cout << "Checksum mismatch for " << model.name << "\n";
			std::cout << "Expected SHA256: " << model.sha256 << "\n";
			if (actual_sha256.has_value()) {
				std::cout << "Actual SHA256:   " << actual_sha256.value() << "\n";
			}
			return kExitAbort;
		}

		const auto install_result = howdy::native::install_staged_file(*staged, model.destination);
		if (!howdy::native::atomic_file_commit_is_durable(install_result)) {
			curl_global_cleanup();
			if (howdy::native::atomic_file_may_have_committed(install_result)) {
				std::cout
				    << "Downloaded model was installed, but its directory could not be synced: "
				    << model.destination.string() << "\n";
			} else {
				std::cout << "Failed to install downloaded model: " << model.destination.string()
				          << "\n";
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
