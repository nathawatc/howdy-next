#include "integration/process.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/acl.h>
#include <sys/file.h>
#include <sys/stat.h>

#include <acl/libacl.h>

namespace fs = std::filesystem;

namespace {
	using Cleanup = howdy::test::TemporaryDirectory;

	struct AclFree {
		auto operator()(void *resource) const -> void {
			acl_free(resource);
		}
	};

	using AclHandle = std::unique_ptr<std::remove_pointer_t<acl_t>, AclFree>;
	using AclBuffer = std::unique_ptr<char, AclFree>;

	auto Require(bool condition, const std::string &message) -> void {
		if (!condition) {
			throw std::runtime_error(message);
		}
	}

	auto ParseId(const std::string &value) -> unsigned {
		std::size_t parsed = 0;
		const auto  result = std::stoul(value, &parsed);
		Require(parsed == value.size(), "invalid numeric identity: " + value);
		return static_cast<unsigned>(result);
	}

	auto Inspect(const fs::path &path, bool directory, unsigned mode, unsigned links = 0)
	    -> struct stat {
		struct stat info{};
		Require(lstat(path.c_str(), &info) == 0, "cannot inspect: " + path.string());
		Require((directory ? S_ISDIR(info.st_mode) : S_ISREG(info.st_mode)) && info.st_uid == 0 &&
		            info.st_gid == 0 && (static_cast<unsigned>(info.st_mode) & 07777U) == mode &&
		            (links == 0 || info.st_nlink == links),
		        "wrong type, owner, mode, or link count: " + path.string());
		return info;

	}

	auto
	AclText(acl_t acl, const char *prefix) -> std::string {
		const AclBuffer formatted(
		    acl_to_any_text(acl, prefix, '\n', TEXT_NUMERIC_IDS | TEXT_SOME_EFFECTIVE));
		if (!formatted) {
			throw std::system_error(errno, std::generic_category(), "cannot format ACL");
		}
		std::string text(formatted.get());
		while (!text.empty() && text.back() == '\n') {
			text.pop_back();
		}
		return text;
	}

	auto Acl(const fs::path &path) -> std::string {
		const auto read = [&](acl_type_t type, const char *prefix) -> std::string {
			const AclHandle acl(acl_get_file(path.c_str(), type));
			if (!acl) {
				throw std::system_error(errno, std::generic_category(),
				                        "cannot inspect ACL: " + path.string());
			}
			return AclText(acl.get(), prefix);
		};
		auto text = read(ACL_TYPE_ACCESS, nullptr);
		if (fs::is_directory(path)) {
			const auto defaults = read(ACL_TYPE_DEFAULT, "default:");
			if (!defaults.empty()) {
				text += '\n' + defaults;
			}
		}
		return text;
	}

	auto RequireAcl(const fs::path &path, const std::string &expected) -> void {
		const auto actual = Acl(path);
		Require(actual == expected, "ACL mismatch: " + path.string() + "\nexpected:\n" + expected +
		                                "\nactual:\n" + actual);
	}

	auto CanRead(const fs::path &path, unsigned uid, unsigned gid, bool init_groups) -> bool {
		const auto result = howdy::test::integration::RunProcess({
		    "setpriv",
		    "--reuid",
		    std::to_string(uid),
		    "--regid",
		    std::to_string(gid),
		    init_groups ? "--init-groups" : "--clear-groups",
		    "cat",
		    path.string(),
		});
		return result.status == 0;
	}

	auto CannotWrite(const fs::path &path, unsigned uid, unsigned gid) -> bool {
		const auto result = howdy::test::integration::RunProcess({
		    "setpriv",
		    "--reuid",
		    std::to_string(uid),
		    "--regid",
		    std::to_string(gid),
		    "--init-groups",
		    "test",
		    "!",
		    "-w",
		    path.string(),
		});
		return result.status == 0;
	}

	// NOLINTBEGIN(bugprone-easily-swappable-parameters)
	auto RunAsUser(const fs::path &program, const std::vector<std::string> &arguments, unsigned uid,
	               unsigned gid, int close_fd = -1) -> howdy::test::integration::ProcessResult {
		// NOLINTEND(bugprone-easily-swappable-parameters)
		std::vector<std::string> command = {
		    "setpriv",           "--reuid",       std::to_string(uid), "--regid",
		    std::to_string(gid), "--init-groups", program.string(),
		};
		command.insert(command.end(), arguments.begin(), arguments.end());
		return howdy::test::integration::RunProcess(std::move(command), {}, close_fd);
	}

	auto VerifyHelperProbes(const fs::path &helper, const std::string &user, unsigned uid,
	                        unsigned gid) -> int {
		const auto invalid = RunAsUser(helper, {"prepare", "../invalid-e2e-user"}, uid, gid);
		Require(invalid.status != 0, "invalid username unexpectedly succeeded");
		if (invalid.output.contains("must be installed setuid root")) {
			return 77;
		}
		Require(invalid.output.contains("Invalid user name"),
		        "setuid preflight did not reach username validation: " + invalid.output);
		const auto wrong_user = RunAsUser(helper, {"prepare", "root"}, uid, gid);
		Require(wrong_user.status != 0, "helper prepared files for different username");
		const auto missing_fd = RunAsUser(helper, {"prepare", user}, uid, gid, 3);
		Require(missing_fd.status != 0, "direct helper prepare succeeded without descriptor 3");
		Require(missing_fd.output ==
		            "howdy-auth-helper requires an inherited lease socket on descriptor 3\n",
		        "direct helper missing-fd error mismatch: " + missing_fd.output);
		return 0;
	}

	auto VerifyInstalled(const fs::path &helper, const fs::path &compare,
	                     const fs::path &config_dir, const fs::path &config, const fs::path &models,
	                     unsigned selected_uid, unsigned selected_gid) -> void {
		Inspect(helper, false, 04755);
		Inspect(compare, false, 0755);
		Inspect(config_dir, true, 0750);
		Inspect(config, false, 0640);
		Inspect(models, true, 0750);
		for (auto parent = helper.parent_path(); !parent.empty(); parent = parent.parent_path()) {
			Require(CannotWrite(parent, selected_uid, selected_gid),
			        "helper parent writable by selected user: " + parent.string());
			if (parent == "/") {
				break;
			}
		}
	}

	auto ReadLines(const fs::path &path) -> std::vector<std::string> {
		std::ifstream input(path);
		Require(input.good(), "cannot read: " + path.string());
		std::vector<std::string> lines;
		for (std::string line; std::getline(input, line);) {
			lines.push_back(std::move(line));
		}
		Require(!input.bad(), "read failed: " + path.string());
		return lines;
	}

	auto ConfigureFixture(const fs::path &paths_header, const fs::path &config,
	                      const fs::path &models, const fs::path &helper, const fs::path &compare)
	    -> void {
		const auto paths = ReadLines(paths_header);
		for (const auto &[key, value] : {
		         std::pair{"kConfiguredConfigPath", config.string()},
		         std::pair{"kConfiguredUserModelsDir", models.string()},
		         std::pair{"kAuthHelperPath", helper.string()},
		         std::pair{"kCompareProcessPath", compare.string()},
		     }) {
			const auto expected =
			    "inline constexpr auto " + std::string(key) + " = \"" + value + "\";";
			Require(std::ranges::find(paths, expected) != paths.end(),
			        "compiled path mismatch: " + std::string(key));
		}
		auto lines = ReadLines(config);
		for (const auto &[key, value] : {
		         std::pair{"disabled", "true"},
		         std::pair{"detection_notice", "true"},
		         std::pair{"no_confirmation", "false"},
		         std::pair{"abort_if_ssh", "false"},
		         std::pair{"abort_if_lid_closed", "false"},
		     }) {
			const std::string prefix = std::string(key) + " = ";
			unsigned          count  = 0;
			for (auto &line : lines) {
				if (line.starts_with(prefix)) {
					line = prefix + value;
					++count;
				}
			}
			Require(count == 1,
			        "expected one " + std::string(key) + " setting in " + config.string());
		}
		std::ofstream output(config, std::ios::trunc);
		Require(output.good(), "cannot update config: " + config.string());
		for (const auto &line : lines) {
			output << line << '\n';
		}
		output.flush();
		Require(output.good(), "cannot write config: " + config.string());
	}

	auto WriteModel(const fs::path &path, const std::string &generation, const std::string &first,
	                const std::string &second) -> void {
		std::ofstream output(path, std::ios::trunc);
		Require(output.good(), "cannot write source model: " + path.string());
		output
		    << R"([{"id":0,"time":)" << generation
		    << R"(,"label":"e2e","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[)"
		    << first << ',' << second << "]]}]\n";
		output.flush();
		Require(output.good(), "cannot write source model: " + path.string());
	}

	auto CheckLock(const fs::path &path, bool should_be_locked) -> void {
		const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
		Require(fd >= 0, "cannot open lease lock: " + path.string());
		const int result      = flock(fd, LOCK_EX | LOCK_NB);
		const int saved_errno = errno;
		close(fd);
		if (should_be_locked) {
			Require(result < 0 && (saved_errno == EWOULDBLOCK || saved_errno == EAGAIN),
			        "active lease permits exclusive lock: " + path.string());
		} else {
			Require(result == 0, "released lease remains locked: " + path.string());
		}
	}

	auto DirectoryAclForUser(unsigned uid) -> std::string {
		return "user::rwx\nuser:" + std::to_string(uid) + ":r-x\ngroup::---\nmask::r-x\nother::---";
	}

	// NOLINTBEGIN(bugprone-easily-swappable-parameters)
	auto VerifyGeneration(const fs::path &generation, const std::string &user,
	                      unsigned selected_uid, unsigned selected_gid, unsigned unrelated_uid,
	                      unsigned unrelated_gid) -> void {
		// NOLINTEND(bugprone-easily-swappable-parameters)
		const auto models_dir = generation / "models";
		const auto config     = generation / "config.ini";
		const auto backing    = generation / ".model_backing";
		const auto visible    = models_dir / (user + ".dat");
		const auto lock       = fs::path(generation.string() + ".lock");
		Inspect(generation, true, 0750);
		Inspect(models_dir, true, 0750);
		Inspect(config, false, 0640, 1);
		Inspect(lock, false, 0600, 1);
		const auto backing_info = Inspect(backing, false, 0640, 2);
		const auto visible_info = Inspect(visible, false, 0640, 2);
		Require(backing_info.st_dev == visible_info.st_dev &&
		            backing_info.st_ino == visible_info.st_ino,
		        "model backing and visible inode differ: " + generation.string());
		const auto directory_acl = DirectoryAclForUser(selected_uid);
		const auto file_acl      = "user::rw-\nuser:" + std::to_string(selected_uid) +
		                           ":r--\ngroup::---\nmask::r--\nother::---";
		RequireAcl(generation, directory_acl);
		RequireAcl(models_dir, directory_acl);
		RequireAcl(config, file_acl);
		RequireAcl(visible, file_acl);
		RequireAcl(backing, file_acl);
		Require(CanRead(config, selected_uid, selected_gid, true) &&
		            CanRead(visible, selected_uid, selected_gid, true),
		        "selected user cannot read staged files: " + generation.string());
		Require(!CanRead(config, unrelated_uid, unrelated_gid, false) &&
		            !CanRead(visible, unrelated_uid, unrelated_gid, false),
		        "unrelated user can read staged files: " + generation.string());
	}

	auto VerifyAbsent(const fs::path &generation, const std::string &user, unsigned selected_uid,
	                  unsigned selected_gid) -> void {
		const auto visible = generation / "models" / (user + ".dat");
		Require(!fs::exists(visible) && !fs::is_symlink(fs::symlink_status(visible)),
		        "absent slot retains visible model: " + visible.string());
		const auto backing = generation / ".model_backing";
		Inspect(backing, false, 0600, 1);
		RequireAcl(backing, "user::rw-\ngroup::---\nother::---");
		Require(!CanRead(backing, selected_uid, selected_gid, true),
		        "absent sentinel became readable: " + backing.string());
	}

	auto CheckAclCapability(const fs::path &parent, unsigned uid) -> int {
		std::string template_path = (parent / "howdy-e2e-acl.XXXXXX").string();
		const char *created       = mkdtemp(template_path.data());
		Require(created != nullptr, "cannot create runtime ACL fixture");
		const Cleanup   cleanup{fs::path(created)};
		const auto      text = DirectoryAclForUser(uid);
		const AclHandle expected(acl_from_text(text.c_str()));
		Require(expected != nullptr, "cannot create runtime ACL");
		const int set_result = acl_set_file(created, ACL_TYPE_ACCESS, expected.get());
		const int set_error  = errno;
		if (set_result != 0) {
			if (set_error == ENOTSUP || set_error == EOPNOTSUPP) {
				return 77;
			}
			throw std::system_error(set_error, std::generic_category(), "cannot set runtime ACL");
		}
		const AclHandle actual(acl_get_file(created, ACL_TYPE_ACCESS));
		const int       get_error = errno;
		if (!actual) {
			if (get_error == ENOTSUP || get_error == EOPNOTSUPP) {
				return 77;
			}
			throw std::system_error(get_error, std::generic_category(), "cannot read runtime ACL");
		}
		const int match       = acl_cmp(actual.get(), expected.get());
		const int match_error = errno;
		if (match < 0) {
			throw std::system_error(match_error, std::generic_category(),
			                        "cannot compare runtime ACL");
		}
		Require(match == 0, "runtime filesystem changed extended ACL");
		return 0;
	}

	auto SelfTest() -> void {
		std::string template_path = "/tmp/howdy-e2e-state-verifier.XXXXXX";
		const char *created       = mkdtemp(template_path.data());
		Require(created != nullptr, "cannot create state-verifier fixture");
		const fs::path root = created;
		const Cleanup  cleanup(root);
		const auto     lock = root / "generation.lock";
		std::ofstream(lock) << "lease";
		const int holder = open(lock.c_str(), O_RDONLY | O_CLOEXEC);
		Require(holder >= 0 && flock(holder, LOCK_EX | LOCK_NB) == 0,
		        "cannot hold self-test lease");
		CheckLock(lock, true);
		Require(flock(holder, LOCK_UN) == 0, "cannot release self-test lease");
		close(holder);
		CheckLock(lock, false);
		const auto check_text = [](const std::string &source, const char *prefix,
		                           const std::string &expected) -> void {
			const AclHandle acl(acl_from_text(source.c_str()));
			Require(acl != nullptr, "cannot create self-test ACL");
			const auto actual = AclText(acl.get(), prefix);
			Require(actual == expected,
			        "ACL text mismatch\nexpected:\n" + expected + "\nactual:\n" + actual);
		};
		constexpr auto access_acl = "user::rwx\nuser:424242:r-x\ngroup::---\nmask::r-x\nother::---";
		check_text(access_acl, nullptr, access_acl);
		check_text("user::rwx\nuser:424242:rwx\ngroup::---\nmask::r-x\nother::---", nullptr,
		           "user::rwx\nuser:424242:rwx\t#effective:r-x\ngroup::---\nmask::r-x\nother::---");
		check_text("user::rwx\ngroup::---\nother::---",
		           "default:", "default:user::rwx\ndefault:group::---\ndefault:other::---");
		std::cout << "State verifier: lock and ACL checks passed\n";
	}

}  // namespace

auto main(int argc, char **argv) -> int {
	std::vector<std::string> args;
	for (const auto *argument : std::span(argv, static_cast<std::size_t>(argc))) {
		args.emplace_back(argument);
	}
	try {
		if (args.size() == 2 && args.at(1) == "--self-test") {
			SelfTest();
		} else if (args.size() == 3 && args.at(1) == "--locked") {
			CheckLock(args.at(2), true);
		} else if (args.size() == 3 && args.at(1) == "--unlocked") {
			CheckLock(args.at(2), false);
		} else if (args.size() == 3 && args.at(1) == "--runtime-root") {
			Inspect(args.at(2), true, 0711);
		} else if (args.size() == 4 && args.at(1) == "--acl-capability") {
			return CheckAclCapability(args.at(2), ParseId(args.at(3)));
		} else if (args.size() == 6 && args.at(1) == "--absent") {
			VerifyAbsent(args.at(2), args.at(3), ParseId(args.at(4)), ParseId(args.at(5)));
		} else if (args.size() == 9 && args.at(1) == "--installed") {
			VerifyInstalled(args.at(2), args.at(3), args.at(4), args.at(5), args.at(6),
			                ParseId(args.at(7)), ParseId(args.at(8)));
		} else if (args.size() == 7 && args.at(1) == "--configure") {
			ConfigureFixture(args.at(2), args.at(3), args.at(4), args.at(5), args.at(6));
		} else if (args.size() == 6 && args.at(1) == "--write-model") {
			WriteModel(args.at(2), args.at(3), args.at(4), args.at(5));
		} else if (args.size() == 6 && args.at(1) == "--helper-probes") {
			return VerifyHelperProbes(args.at(2), args.at(3), ParseId(args.at(4)),
			                          ParseId(args.at(5)));
		} else if (args.size() == 8 && args.at(1) == "--generation") {
			VerifyGeneration(args.at(2), args.at(3), ParseId(args.at(4)), ParseId(args.at(5)),
			                 ParseId(args.at(6)), ParseId(args.at(7)));
		} else {
			std::cerr << "invalid state-verifier arguments\n";
			return 2;
		}
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
