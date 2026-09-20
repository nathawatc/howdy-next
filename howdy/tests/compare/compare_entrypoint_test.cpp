#include "compare/entrypoint.hpp"
#include "protocol/compare_exit.hpp"
#include "support/file_security/validation_root.hpp"
#include "test_support.hpp"

#include <array>
#include <filesystem>
#include <string>
#include <system_error>

#include <stdlib.h>
#include <unistd.h>

#ifndef HOWDY_PACKAGED_CONFIG_PATH
#	error "HOWDY_PACKAGED_CONFIG_PATH must be defined by CMake"
#endif
#ifndef HOWDY_COMPARE_NO_MODEL_ROOT
#	error "HOWDY_COMPARE_NO_MODEL_ROOT must be defined by CMake"
#endif

namespace {

	namespace fs = std::filesystem;

	auto PrepareFixture(const fs::path &root) -> bool {
		std::error_code error;
		fs::remove_all(root, error);
		error.clear();
		if (!fs::create_directories(root / "models", error) || error) {
			return false;
		}
		fs::copy_file(HOWDY_PACKAGED_CONFIG_PATH, root / "config.ini",
		              fs::copy_options::overwrite_existing, error);
		if (error) {
			return false;
		}
		fs::permissions(root,
		                fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
		                    fs::perms::others_read | fs::perms::others_exec,
		                fs::perm_options::replace, error);
		if (error) {
			return false;
		}
		fs::permissions(root / "models",
		                fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
		                    fs::perms::others_read | fs::perms::others_exec,
		                fs::perm_options::replace, error);
		if (error) {
			return false;
		}
		fs::permissions(root / "config.ini",
		                fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
		                    fs::perms::others_read,
		                fs::perm_options::replace, error);
		return !error;
	}

}  // namespace

auto main() -> int {
	using howdy::test::Expect;

	const fs::path root(HOWDY_COMPARE_NO_MODEL_ROOT);
	if (!Expect(PrepareFixture(root), "prepare secure compare no-model fixture")) {
		return 1;
	}

	const auto config_path = (root / "config.ini").string();
	const auto models_path = (root / "models").string();
	bool       ok          = true;
	ok &= Expect(setenv("HOWDY_CONFIG", config_path.c_str(), 1) == 0, "set compare config path");
	ok &= Expect(setenv("HOWDY_USER_MODELS_DIR", models_path.c_str(), 1) == 0,
	             "set empty user-model directory");

	std::array<std::string, 2> arguments{"howdy-compare", "coverage-user"};
	std::array<char *, 2>      argv{arguments.at(0).data(), arguments.at(1).data()};
	const int                  result = howdy::native::RunCompareEntrypoint(
	    static_cast<int>(argv.size()), argv.data(), geteuid(),
	    howdy::native::file_security_internal::ValidationRoot{.path = root});
	ok &= Expect(result == static_cast<int>(howdy::native::CompareExit::kNoFaceModel),
	             "empty secure user-model directory returns kNoFaceModel");

	std::error_code cleanup_error;
	fs::remove_all(root, cleanup_error);
	ok &= Expect(!cleanup_error, "clean compare no-model fixture");
	return ok ? 0 : 1;
}
