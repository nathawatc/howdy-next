#pragma once

#include "common/atomic_files.hpp"
#include "common/opencv_model_manifest.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>

#include <sys/stat.h>
#include <sys/types.h>

#include <curl/curl.h>

namespace howdy::native::download_models_internal {

	using StagedDownloadFile                       = StagedFile;
	inline constexpr std::size_t kMaxDownloadBytes = std::size_t{100} * 1024 * 1024;

	struct DownloadWriteContext {
		StagedDownloadFile *staged        = nullptr;
		std::size_t         bytes_written = 0;
		std::size_t         max_bytes     = kMaxDownloadBytes;
	};

	using DownloadFileFn      = bool (*)(const std::string &url, StagedDownloadFile &staged);
	using ModelFileOwnerUidFn = std::optional<uid_t> (*)();
	using Sha256FileFn        = std::optional<std::string> (*)(int fd);
	using FstatFn             = int (*)(int fd, struct stat *stat_buf);

	struct CurlSetoptOperations {
		void *context = nullptr;

		CURLcode (*set_long)(void *context, CURL *curl, CURLoption option, long value) = nullptr;
		CURLcode (*set_off_t)(void *context, CURL *curl, CURLoption option,
		                      curl_off_t value)                                        = nullptr;
		CURLcode (*set_string)(void *context, CURL *curl, CURLoption option,
		                       const char *value)                                      = nullptr;
	};

	[[nodiscard]] auto sha256_file_descriptor(int fd) -> std::optional<std::string>;
	[[nodiscard]] auto configure_transfer_policy(CURL *curl, const CurlSetoptOperations &operations)
	    -> bool;

	struct DownloadModelsDependencies {
		DownloadFileFn                         download_file;
		ModelFileOwnerUidFn                    model_file_owner_uid;
		Sha256FileFn                           sha256_file = sha256_file_descriptor;
		FstatFn                                fstat_file  = ::fstat;
		std::span<const OpenCvModelDescriptor> models      = official_opencv_models();
	};

	auto download_models_write_callback(void *contents, std::size_t size, std::size_t nmemb,
	                                    void *userp) -> std::size_t;

	auto download_models_main_with_dependencies(int argc, char **argv,
	                                            const DownloadModelsDependencies &dependencies)
	    -> int;

}  // namespace howdy::native::download_models_internal
