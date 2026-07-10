#pragma once

#include "common/atomic_files.hpp"

#include <cstddef>
#include <optional>
#include <string>

#include <sys/types.h>

namespace howdy::native::download_models_internal {

	using StagedDownloadFile                       = StagedFile;
	inline constexpr std::size_t kMaxDownloadBytes = 100U * 1024U * 1024U;

	struct DownloadWriteContext {
		StagedDownloadFile *staged        = nullptr;
		std::size_t         bytes_written = 0;
		std::size_t         max_bytes     = kMaxDownloadBytes;
	};

	using DownloadFileFn      = bool (*)(const std::string &url, StagedDownloadFile &staged);
	using ModelFileOwnerUidFn = std::optional<uid_t> (*)();

	struct DownloadModelsDependencies {
		DownloadFileFn      download_file;
		ModelFileOwnerUidFn model_file_owner_uid;
	};

	auto download_models_write_callback(void *contents, std::size_t size, std::size_t nmemb,
	                                    void *userp) -> std::size_t;

	auto download_models_main_with_dependencies(int argc, char **argv,
	                                            const DownloadModelsDependencies &dependencies)
	    -> int;

}  // namespace howdy::native::download_models_internal
