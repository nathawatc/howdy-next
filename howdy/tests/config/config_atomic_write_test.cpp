#include "config/config_utils.hpp"
#include "config/config_utils_test_support.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <string>
#include <vector>

#include <sys/resource.h>

namespace howdy::test {

	auto RunConfigAtomicWriteTests(ConfigUtilsTestContext &context) -> bool {
		namespace fs = std::filesystem;
		using howdy::test::expect;
		bool                           ok = true;
		std::error_code                ec;
		const auto                     nested_path = context.temp_root / "nested" / "generated.ini";
		const std::vector<std::string> write_lines = {
		    "[face]\n",
		    "sface_threshold = 0.363\n",
		};
		ok &= expect(howdy::native::AtomicFileCommitIsDurable(
		                 howdy::native::AtomicWriteLines(nested_path, write_lines)),
		             "atomic_write_lines creates parent dirs and writes file");
		ok &= expect(ReadConfigTestFile(nested_path) == "[face]\nsface_threshold = 0.363\n",
		             "atomic_write_lines output matches expected content");
		const auto atomic_directory_path = context.temp_root / "atomic-directory.ini";
		ok &= expect(fs::create_directory(atomic_directory_path, ec),
		             "create non-regular atomic write target");
		ok &= expect(howdy::native::AtomicWriteLines(atomic_directory_path, write_lines) ==
		                 howdy::native::AtomicFileCommitResult::kNotCommitted,
		             "atomic_write_lines rejects non-regular target");
		const auto         atomic_size_path = context.temp_root / "atomic-size-limit.ini";
		FileSizeLimitGuard atomic_file_size_limit;
		const bool         atomic_limit_set = atomic_file_size_limit.SetZero();
		if (atomic_limit_set) {
			ok &= expect(howdy::native::AtomicWriteLines(atomic_size_path, write_lines) ==
			                 howdy::native::AtomicFileCommitResult::kNotCommitted,
			             "atomic_write_lines reports staged write failure");
			ok &= expect(atomic_file_size_limit.Restore(),
			             "restore file-size limit after atomic write failure");
		}
		const auto uncertain_lines_path = context.temp_root / "uncertain-lines.ini";
		const auto uncertain_lines_result =
		    howdy::native::AtomicWriteLines(uncertain_lines_path, write_lines, FailParentSync);
		ok &= expect(uncertain_lines_result ==
		                 howdy::native::AtomicFileCommitResult::kCommittedSyncFailed,
		             "atomic_write_lines reports committed parent-sync failure");
		ok &=
		    expect(ReadConfigTestFile(uncertain_lines_path) == "[face]\nsface_threshold = 0.363\n",
		           "atomic_write_lines leaves committed content visible after sync failure");

		return ok;
	}

}  // namespace howdy::test
