#pragma once

#include "common/atomic_files.hpp"

#include <optional>
#include <string>

#include <sys/types.h>

namespace howdy::native::download_models_internal {

	using StagedDownloadFile = StagedFile;

	using DownloadFileFn      = bool (*)(const std::string &url, StagedDownloadFile &staged);
	using ModelFileOwnerUidFn = std::optional<uid_t> (*)();

	struct DownloadModelsDependencies {
		DownloadFileFn      download_file;
		ModelFileOwnerUidFn model_file_owner_uid;
	};

	auto download_models_main_with_dependencies(int argc, char **argv,
	                                            const DownloadModelsDependencies &dependencies)
	    -> int;

}  // namespace howdy::native::download_models_internal
