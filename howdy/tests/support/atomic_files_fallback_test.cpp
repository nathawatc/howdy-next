#include <sys/syscall.h>

#include <linux/fs.h>

#undef SYS_renameat2
#undef RENAME_NOREPLACE

#include "support/atomic_files.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <string>
#include <unistd.h>

namespace {

	auto TestNoReplaceFallback() -> bool {
		namespace fs = std::filesystem;

		bool       ok = true;
		const auto root =
		    fs::temp_directory_path() / ("howdy-atomic-files-fallback-" + std::to_string(getpid()));
		std::error_code ec;
		fs::remove_all(root, ec);
		ec.clear();
		fs::create_directories(root, ec);
		ok &= howdy::test::Expect(!ec, "creates fallback test directory");
		if (ec) {
			return false;
		}

		const auto destination = root / "destination";
		auto       staged      = howdy::native::PrepareStagedFile(destination, ".howdy-fallback-");
		ok &= howdy::test::Expect(staged.has_value(), "prepares staged fallback file");
		if (staged.has_value()) {
			const auto staged_path = staged->path;
			const auto result      = howdy::native::InstallStagedFile(
			    *staged, destination, howdy::native::SyncParentDirectory,
			    howdy::native::AtomicFileInstallPolicy::kNoReplaceExisting);
			ok &=
			    howdy::test::Expect(result == howdy::native::AtomicFileCommitResult::kNotCommitted,
			                        "no-replace fallback reports not committed");
			ok &= howdy::test::Expect(!fs::exists(destination),
			                          "no-replace fallback does not create destination");
			ok &= howdy::test::Expect(!fs::exists(staged_path),
			                          "no-replace fallback removes staged file");
		}

		fs::remove_all(root, ec);
		return ok;
	}

}  // namespace

auto main() -> int {
	return TestNoReplaceFallback() ? 0 : 1;
}
