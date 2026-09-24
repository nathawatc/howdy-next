#include "integration/process.hpp"

#include <array>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;

namespace {

	auto Require(bool condition, const std::string &message) -> void {
		if (!condition) {
			throw std::runtime_error(message);
		}
	}

	auto Read(const fs::path &path) -> std::string {
		std::ifstream input(path, std::ios::binary);
		Require(input.good(), "cannot read: " + path.string());
		return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	}

	auto StripSlashes(std::string path) -> std::string {
		while (path.size() > 1 && path.back() == '/') {
			path.pop_back();
		}
		return path;
	}

	auto Resolve(std::string_view prefix, std::string_view directory) -> std::string {
		if (directory.starts_with('/')) {
			return StripSlashes(std::string(directory));
		}
		return std::string(prefix == "/" ? "" : prefix) + "/" +
		       StripSlashes(std::string(directory));
	}

	auto Staged(const fs::path &stage, const std::string &absolute) -> fs::path {
		return stage.string() + absolute;
	}

	auto Mode(const fs::path &path) -> unsigned {
		return static_cast<unsigned>(fs::status(path).permissions()) & 07777U;
	}

	auto RequireMode(const fs::path &path, unsigned expected) -> void {
		const auto         actual = Mode(path);
		std::ostringstream message;
		message << "wrong mode: " << path << " (expected " << std::oct << expected << ", got "
		        << actual << ')';
		Require(actual == expected, message.str());
	}

	// NOLINTBEGIN(cppcoreguidelines-special-member-functions,hicpp-special-member-functions,misc-non-private-member-variables-in-classes)
	struct Fixture {
		fs::path                    build;
		std::string                 prefix;
		std::array<std::string, 12> directories;
		unsigned                    auth_helper_mode;
		std::vector<fs::path>       cleanup;

		~Fixture() {
			for (const auto &path : cleanup) {
				std::error_code error;
				fs::remove_all(path, error);
			}
		}

		[[nodiscard]] auto Path(std::size_t index) const -> std::string {
			return Resolve(prefix, directories.at(index));
		}

		[[nodiscard]] static auto Install(const fs::path &stage, const fs::path &directory,
		                                  const std::string &override_prefix = {})
		    -> howdy::test::integration::ProcessResult {
			std::vector<std::string> args = {"cmake", "--install", directory.string()};
			if (!override_prefix.empty()) {
				args.insert(args.end(), {"--prefix", override_prefix});
			}
			return howdy::test::integration::RunProcess(std::move(args),
			                                            {{"DESTDIR", stage.string()}});
		}

		static auto RequireInstall(const fs::path &stage, const fs::path &directory,
		                           const std::string &override_prefix = {}) -> void {
			const auto result = Install(stage, directory, override_prefix);
			Require(result.status == 0,
			        "install failed for " + directory.string() + ": " + result.output);
		}

		auto VerifyLayout(const fs::path &stage) const -> void {
			const auto bindir          = Path(0);
			const auto libexecdir      = Path(1);
			const auto datadir         = Path(2);
			const auto localedir       = Path(3);
			const auto mandir          = Path(4);
			const auto helper_dir      = Path(5);
			const auto pam_dir         = Path(6);
			const auto config_dir      = Path(7);
			const auto config_path     = Path(8);
			const auto models_dir      = Path(9);
			const auto user_models_dir = Path(10);
			const auto licenses_dir    = Path(11);
			Require(helper_dir == libexecdir + "/howdy",
			        "helper directory differs from libexecdir/howdy: " + helper_dir);

			std::set<fs::path> files;
			Require(fs::is_directory(fs::symlink_status(stage)),
			        "staging root is not a real directory: " + stage.string());
			std::set<fs::path> directories_found{stage};
			for (const auto &entry : fs::recursive_directory_iterator(stage)) {
				const auto status = fs::symlink_status(entry.path());
				if (fs::is_regular_file(status)) {
					files.insert(entry.path());
				} else if (fs::is_directory(status)) {
					directories_found.insert(entry.path());
				}
			}
			const auto howdy       = Staged(stage, bindir + "/howdy");
			const auto compare     = Staged(stage, helper_dir + "/howdy-compare");
			const auto auth_helper = Staged(stage, helper_dir + "/howdy-auth-helper");
			const auto config      = Staged(stage, config_path);
			for (const auto &path :
			     {howdy, compare, auth_helper, Staged(stage, pam_dir + "/pam_howdy.so"), config,
			      Staged(stage, datadir + "/bash-completion/completions/howdy"),
			      Staged(stage, localedir + "/th/LC_MESSAGES/howdy.mo"),
			      Staged(stage, mandir + "/man1/howdy.1"),
			      Staged(stage, mandir + "/man5/howdy.ini.5"),
			      Staged(stage, mandir + "/man8/pam_howdy.8"),
			      Staged(stage, licenses_dir + "/LICENSE"),
			      Staged(stage, licenses_dir + "/THIRD_PARTY_NOTICES.md"),
			      Staged(stage, licenses_dir + "/YUNET-MIT.txt"),
			      Staged(stage, licenses_dir + "/SFACE-APACHE-2.0.txt")}) {
				Require(fs::is_regular_file(fs::symlink_status(path)) && files.contains(path),
				        "missing installed regular file: " + path.string());
			}
			for (const auto &path : files) {
				Require(path.filename() != "howdy_docs_generator",
				        "documentation generator was installed: " + path.string());
			}
			for (const auto &path : {howdy, compare, auth_helper}) {
				Require(access(path.c_str(), X_OK) == 0,
				        "installed executable is not executable: " + path.string());
			}
			for (const auto &path : {Staged(stage, models_dir), Staged(stage, user_models_dir),
			                         Staged(stage, config_dir)}) {
				Require(fs::is_directory(fs::symlink_status(path)) &&
				            directories_found.contains(path),
				        "missing installed directory: " + path.string());
			}
			RequireMode(auth_helper, auth_helper_mode);
			RequireMode(config, 0640);
			RequireMode(Staged(stage, config_dir), 0750);
			RequireMode(Staged(stage, user_models_dir), 0750);
			for (const auto *command : {"add", "clear", "config", "disable", "download-models",
			                            "list", "remove", "set", "snapshot", "test"}) {
				for (const auto &directory : {bindir, helper_dir}) {
					const auto obsolete = Staged(stage, directory + "/howdy-" + command);
					Require(!fs::exists(obsolete),
					        "obsolete standalone CLI command installed: " + obsolete.string());
				}
			}
		}
	};

	// NOLINTEND(cppcoreguidelines-special-member-functions,hicpp-special-member-functions,misc-non-private-member-variables-in-classes)

	auto RequireRejected(const fs::path &directory, const fs::path &stage,
	                     const std::string &different_prefix) -> void {
		fs::remove_all(stage);
		const auto result = Fixture::Install(stage, directory, different_prefix);
		Require(result.status != 0,
		        "install unexpectedly accepted prefix override: " + directory.string());
		for (const auto *message :
		     {"Reconfigure with -DCMAKE_INSTALL_PREFIX=<path>", "cmake --install --prefix",
		      "runtime paths are generated at configure time"}) {
			Require(result.output.contains(message),
			        "prefix rejection omitted '" + std::string(message) + "': " + result.output);
		}
		Require(!fs::exists(stage), "prefix rejection created artifacts: " + stage.string());
	}

}  // namespace

auto main(int argc, char **argv) -> int {
	if (argc != 16) {
		std::cerr << "usage: install_layout_test BUILD PREFIX BINDIR LIBEXECDIR DATADIR "
		             "LOCALEDIR MANDIR HELPER_DIR PAM_DIR CONFIG_DIR CONFIG_PATH MODELS_DIR "
		             "USER_MODELS_DIR LICENSES_DIR AUTH_HELPER_MODE\n";
		return 2;
	}
	try {
		const auto args = howdy::test::integration::CopyArguments(argc, argv);
		Fixture fixture{.build       = args.at(1),
		                .prefix      = StripSlashes(args.at(2)),
		                .directories = {args.at(3), args.at(4), args.at(5), args.at(6), args.at(7),
		                                args.at(8), args.at(9), args.at(10), args.at(11),
		                                args.at(12), args.at(13), args.at(14)},
		                .auth_helper_mode =
		                    static_cast<unsigned>(std::stoul(args.at(15), nullptr, 8)),
		                .cleanup = {}};
		Require(Resolve("/", "bin") == "/bin" && Resolve("/", "/etc") == "/etc",
		        "root install prefix produced invalid destination path");
		const auto stage          = fixture.build / "install layout root-ทดสอบ";
		const auto same_stage     = fixture.build / "install same prefix root-ทดสอบ";
		const auto override_stage = fixture.build / "install override root-ทดสอบ";
		const auto literal_root   = fixture.build / "install literal prefix cases";
		fixture.cleanup           = {stage, same_stage, override_stage, literal_root};
		fs::remove_all(literal_root);
		for (const auto &[name, prefix] : {std::pair{"dollar", "/opt/howdy/$literal"},
		                                   std::pair{"braced-dollar", "/opt/howdy/${UNDEFINED}"},
		                                   std::pair{"quote", "/opt/howdy/\"quoted\""}}) {
			const auto case_dir = literal_root / name;
			fs::create_directories(case_dir);
			fs::copy_file(fixture.build / "cmake_install_prefix_guard.cmake",
			              case_dir / "cmake_install_prefix_guard.cmake");
			std::ofstream(case_dir / "cmake_install_prefix.txt") << prefix;
			const auto result = howdy::test::integration::RunProcess(
			    {"cmake", "-D", std::string("CMAKE_INSTALL_PREFIX=") + prefix, "-P",
			     (case_dir / "cmake_install_prefix_guard.cmake").string()});
			Require(result.status == 0,
			        "literal prefix rejected: " + std::string(prefix) + ": " + result.output);
		}

		fs::remove_all(stage);
		Fixture::RequireInstall(stage, fixture.build);
		fixture.VerifyLayout(stage);
		const auto        installed_config = Staged(stage, fixture.Path(8));
		const std::string marker           = "install-layout-config-preservation-marker\n";
		std::ofstream(installed_config) << marker;
		Fixture::RequireInstall(stage, fixture.build);
		fixture.VerifyLayout(stage);
		Require(Read(installed_config) == marker,
		        "second staged install overwrote existing config.ini: " +
		            installed_config.string());
		for (const auto &path : {fs::path(stage.string() + stage.string()),
		                         fs::path(stage.string() + stage.string() + fixture.Path(8)),
		                         fs::path(stage.string() + stage.string() + fixture.Path(10))}) {
			Require(!fs::exists(path), "doubled DESTDIR path exists: " + path.string());
		}
		std::size_t config_count = 0;
		for (const auto &entry : fs::recursive_directory_iterator(stage)) {
			if (fs::is_regular_file(fs::symlink_status(entry.path())) &&
			    entry.path().filename() == "config.ini") {
				++config_count;
			}
		}
		Require(config_count == 1,
		        "expected one config.ini in staged tree, found " + std::to_string(config_count));

		const auto same_prefixes =
		    fixture.prefix == "/"
		        ? std::vector<std::string>{"/."}
		        : std::vector<std::string>{fixture.prefix + "/", fixture.prefix + "//",
		                                   fixture.prefix + "/.", "/" + fixture.prefix};
		for (const auto &prefix : same_prefixes) {
			fs::remove_all(same_stage);
			Fixture::RequireInstall(same_stage, fixture.build, prefix);
			fixture.VerifyLayout(same_stage);
		}

		std::string different_prefix = (fixture.build / "install-prefix-override-target").string();
		while (StripSlashes(different_prefix) == fixture.prefix) {
			different_prefix += "-different";
		}
		for (const auto &[directory, name] :
		     {std::pair{fixture.build / "pam", "direct pam install root-ทดสอบ"},
		      std::pair{fixture.build / "howdy", "direct howdy install root-ทดสอบ"},
		      std::pair{fixture.build / "pam/po", "direct pam-po install root-ทดสอบ"}}) {
			const auto direct_stage = fixture.build / name;
			fixture.cleanup.push_back(direct_stage);
			RequireRejected(directory, direct_stage, different_prefix);
		}
		RequireRejected(fixture.build, override_stage, different_prefix);
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
