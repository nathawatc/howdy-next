#include "auth_helper/auth_helper_test_groups.hpp"
#include "auth_helper/command.hpp"
#include "auth_helper/runtime/internal.hpp"
#include "protocol/auth_helper_protocol.hpp"
#include "test_support.hpp"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <grp.h>
#include <iostream>
#include <optional>
#include <pwd.h>
#include <sstream>
#include <string>
#include <tuple>
#include <unistd.h>

#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

namespace {
	using howdy::native::auth_helper::PreparedPaths;
	using howdy::native::auth_helper::internal::RuntimeSources;
	using howdy::native::auth_helper::internal::StagedIdentity;
	using howdy::test::expect;
	using howdy::test::read_file;
	using howdy::test::write_file;

	struct Fixture {
		std::filesystem::path root;
		std::filesystem::path source;
		std::filesystem::path config;
		std::filesystem::path models;
		StagedIdentity        identity;
		RuntimeSources        sources;
	};

	auto close_lease(std::optional<PreparedPaths> &prepared) -> void {
		if (prepared.has_value() && prepared->lease_fd >= 0) {
			(void)close(prepared->lease_fd);
			prepared->lease_fd = -1;
		}
	}

	auto inode_of(const std::filesystem::path &path) -> std::optional<std::pair<dev_t, ino_t>> {
		struct stat stat_{};
		if (lstat(path.c_str(), &stat_) != 0) {
			return std::nullopt;
		}
		return std::pair{stat_.st_dev, stat_.st_ino};
	}

	auto child_access_matches(const PreparedPaths &prepared, uid_t uid, gid_t gid,
	                          bool expected_access) -> std::optional<bool> {
		const pid_t child = fork();
		if (child == 0) {
			if (setgroups(0, nullptr) != 0 || setgid(gid) != 0 || setuid(uid) != 0) {
				_exit(2);
			}
			const std::array descriptors = {
			    open(prepared.runtime_dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC),
			    open(prepared.user_models_dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC),
			    open(prepared.config_path.c_str(), O_RDONLY | O_CLOEXEC),
			    open((prepared.user_models_dir / "alice.dat").c_str(), O_RDONLY | O_CLOEXEC),
			};
			bool matches = true;
			for (const int fd : descriptors) {
				matches = matches && ((fd >= 0) == expected_access);
				if (fd >= 0) {
					(void)close(fd);
				}
			}
			_exit(matches ? 0 : 1);
		}
		if (child < 0) {
			return std::nullopt;
		}
		int   status = 0;
		pid_t waited;
		do {
			waited = waitpid(child, &status, 0);
		} while (waited < 0 && errno == EINTR);
		if (waited != child || !WIFEXITED(status) || WEXITSTATUS(status) == 2) {
			return std::nullopt;
		}
		return WEXITSTATUS(status) == 0;
	}

	auto make_fixture(const std::filesystem::path &temp_root, std::string_view name,
	                  uid_t target_uid) -> std::optional<Fixture> {
		namespace fs = std::filesystem;
		Fixture fixture;
		fixture.root     = temp_root / (std::string(name) + "-runtime");
		fixture.source   = temp_root / (std::string(name) + "-source");
		fixture.config   = fixture.source / "config.ini";
		fixture.models   = fixture.source / "models";
		fixture.identity = {
		    .target_uid = target_uid, .owner_uid = geteuid(), .owner_gid = getegid()};
		fixture.sources = {
		    .runtime_root    = fixture.root,
		    .config          = "./config.ini",
		    .user_models_dir = "./models",
		};
		std::error_code ec;
		fs::remove_all(fixture.root, ec);
		ec.clear();
		fs::remove_all(fixture.source, ec);
		ec.clear();
		fs::create_directories(fixture.models, ec);
		if (ec || chmod(fixture.source.c_str(), 0755) != 0 ||
		    chmod(fixture.models.c_str(), 0755) != 0 ||
		    !write_file(fixture.config, "config-v1\n") ||
		    chmod(fixture.config.c_str(), 0644) != 0) {
			return std::nullopt;
		}
		return fixture;
	}

	auto prepare(const Fixture                                   &fixture,
	             const howdy::native::auth_helper::AclOperations &operations)
	    -> std::optional<PreparedPaths> {
		std::error_code ec;
		const auto      previous = std::filesystem::current_path();
		std::filesystem::current_path(fixture.source, ec);
		if (ec) {
			return std::nullopt;
		}
		auto result = howdy::native::auth_helper::internal::prepare_runtime_auth_files(
		    "alice", fixture.identity, fixture.sources, operations);
		std::filesystem::current_path(previous, ec);
		return ec ? std::nullopt : std::move(result);
	}

	auto expect_protocol_paths() -> bool {
		using namespace howdy::native::auth_helper_protocol;
		bool ok = true;
		for (const auto &[name, expected_uid, expected_slot, valid] : {
		         std::tuple{"pam-0-gen000", uid_t{0}, RuntimeGenerationSlot::kSlot0, true},
		         std::tuple{"pam-42-gen001", uid_t{42}, RuntimeGenerationSlot::kSlot1, true},
		         std::tuple{"pam-042-gen000", uid_t{0}, RuntimeGenerationSlot::kSlot0, false},
		         std::tuple{"pam--gen000", uid_t{0}, RuntimeGenerationSlot::kSlot0, false},
		         std::tuple{"pam-42-gen002", uid_t{0}, RuntimeGenerationSlot::kSlot0, false},
		         std::tuple{"pam-42-gen000x", uid_t{0}, RuntimeGenerationSlot::kSlot0, false},
		     }) {
			uid_t                 uid = 0;
			RuntimeGenerationSlot slot{};
			const bool            parsed = parse_runtime_generation_name(name, &uid, &slot);
			ok &= expect(parsed == valid, std::string("generation parser classification: ") + name);
			if (valid) {
				ok &= expect(uid == expected_uid && slot == expected_slot,
				             std::string("generation parser value: ") + name);
			}
		}
		ok &= expect(prepared_runtime_generation_name(1000, RuntimeGenerationSlot::kSlot0) ==
		                 "pam-1000-gen000",
		             "slot 0 name is exact");
		ok &= expect(prepared_runtime_generation_name(1000, RuntimeGenerationSlot::kSlot1) ==
		                 "pam-1000-gen001",
		             "slot 1 name is exact");

		std::ostringstream output;
		auto              *previous = std::cout.rdbuf(output.rdbuf());
		howdy::native::auth_helper::command::print_prepared_paths(
		    "/run/howdy/pam-1000-gen000/config.ini", "/run/howdy/pam-1000-gen000/models");
		std::cout.rdbuf(previous);
		ok &= expect(output.str() == "CONFIG_PATH=/run/howdy/pam-1000-gen000/config.ini\n"
		                             "USER_MODELS_DIR=/run/howdy/pam-1000-gen000/models\n",
		             "stdout protocol remains exact");
		return ok;
	}

	auto expect_cross_uid_acl_access(
	    const std::filesystem::path                     &temp_root,
	    const howdy::native::auth_helper::AclOperations &production_operations) -> bool {
		if (geteuid() != 0 || getuid() != geteuid()) {
			std::cerr << "SKIP: cross-UID staged access checks require real root\n";
			return true;
		}
		constexpr uid_t target_uid    = 61001;
		constexpr uid_t unrelated_uid = 61002;
		constexpr gid_t shared_gid    = 61003;
		constexpr gid_t unrelated_gid = 61004;
		auto            fixture       = make_fixture(temp_root, "acl-access", target_uid);
		if (!fixture.has_value() || !write_file(fixture->models / "alice.dat", "model-content\n") ||
		    chmod((fixture->models / "alice.dat").c_str(), 0644) != 0) {
			return false;
		}
		auto prepared = prepare(*fixture, production_operations);
		bool ok       = expect(prepared.has_value(), "real ACL preparation succeeds");
		if (!prepared.has_value()) {
			return false;
		}
		for (const auto &[uid, gid, expected_access, label] : {
		         std::tuple{target_uid, shared_gid, true,
		                    "named target UID traverses and reads staged files"},
		         std::tuple{unrelated_uid, shared_gid, false,
		                    "unrelated UID with same GID cannot access staged files"},
		         std::tuple{unrelated_uid, unrelated_gid, false,
		                    "unrelated UID and GID cannot access staged files"},
		     }) {
			const auto result = child_access_matches(*prepared, uid, gid, expected_access);
			if (!result.has_value()) {
				std::cerr << "SKIP: cross-UID credential transition unavailable\n";
				break;
			}
			ok &= expect(*result, label);
		}
		close_lease(prepared);
		return ok;
	}

	auto count_runtime_generations(const std::filesystem::path &root) -> std::size_t {
		using namespace howdy::native::auth_helper_protocol;
		std::size_t count = 0;
		for (const auto &entry : std::filesystem::directory_iterator(root)) {
			uid_t                 uid = 0;
			RuntimeGenerationSlot slot{};
			if (parse_runtime_generation_name(entry.path().filename().string(), &uid, &slot)) {
				++count;
			}
		}
		return count;
	}

	auto exclusive_lock_is_contended(int fd) -> bool {
		errno = 0;
		return flock(fd, LOCK_EX | LOCK_NB) != 0 && (errno == EWOULDBLOCK || errno == EAGAIN);
	}

	auto
	expect_read_only_lease(const std::optional<howdy::native::auth_helper::PreparedPaths> &prepared)
	    -> bool {
		if (!prepared.has_value() || prepared->lease_fd < 0) {
			return expect(false, "first prepare returns lease");
		}

		bool ok = true;
		ok &= expect((fcntl(prepared->lease_fd, F_GETFD) & FD_CLOEXEC) != 0,
		             "returned lease is close-on-exec");
		ok &= expect((fcntl(prepared->lease_fd, F_GETFL) & O_ACCMODE) == O_RDONLY,
		             "returned lease is read-only");
		errno                     = 0;
		const bool write_rejected = write(prepared->lease_fd, "x", 1) == -1 && errno == EBADF;
		ok &= expect(write_rejected, "returned lease rejects writes");
		errno = 0;
		const bool truncate_rejected =
		    ftruncate(prepared->lease_fd, 1) == -1 && (errno == EINVAL || errno == EBADF);
		ok &= expect(truncate_rejected, "returned lease rejects truncation");
		return ok;
	}

	auto expect_reuse_and_inode_bound(const Fixture                                   &fixture,
	                                  const howdy::native::auth_helper::AclOperations &operations)
	    -> bool {
		using namespace howdy::native::auth_helper_protocol;
		bool ok    = true;
		auto first = prepare(fixture, operations);
		ok &= expect_read_only_lease(first);
		if (!first.has_value()) {
			return false;
		}
		const auto config_inode  = inode_of(first->config_path);
		const auto backing_path  = first->runtime_dir / kPreparedModelBackingFileName;
		const auto backing_inode = inode_of(backing_path);
		ok &= expect(count_runtime_generations(fixture.root) == 2,
		             "exactly two fixed generation directories exist");
		ok &= expect(config_inode.has_value() && backing_inode.has_value(),
		             "persistent config and backing inodes exist");

		auto second = prepare(fixture, operations);
		ok &= expect(second.has_value() && second->runtime_dir == first->runtime_dir,
		             "fresh prepare reuses same slot");
		if (!second.has_value()) {
			close_lease(first);
			return false;
		}
		ok &= expect(inode_of(second->config_path) == config_inode &&
		                 inode_of(second->runtime_dir / kPreparedModelBackingFileName) ==
		                     backing_inode,
		             "fresh prepare reuses exact backing inodes");
		for (int attempt = 0; attempt < 16; ++attempt) {
			auto repeated = prepare(fixture, operations);
			ok &= expect(repeated.has_value() && repeated->runtime_dir == first->runtime_dir &&
			                 inode_of(repeated->config_path) == config_inode &&
			                 inode_of(repeated->runtime_dir / kPreparedModelBackingFileName) ==
			                     backing_inode,
			             "repeated prepare preserves fixed slot and backing inodes");
			close_lease(repeated);
		}

		const auto lock_path = prepared_runtime_generation_lock_path(first->runtime_dir);
		const int  probe     = open(lock_path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
		ok &= expect(probe >= 0, "opens independent lease probe");
		if (probe >= 0) {
			ok &= expect(exclusive_lock_is_contended(probe),
			             "active returned leases hold shared locks");
			close_lease(first);
			ok &= expect(exclusive_lock_is_contended(probe),
			             "second lease uses fresh open file description");
			close_lease(second);
			ok &= expect(flock(probe, LOCK_EX | LOCK_NB) == 0, "last lease close releases slot");
			(void)flock(probe, LOCK_UN);
			(void)close(probe);
		}

		ok &= expect(write_file(fixture.config, "config-v2-longer\n"), "updates source config");
		const int map_fd = open(first->config_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		void     *mapping =
		    map_fd < 0 ? MAP_FAILED : mmap(nullptr, 10, PROT_READ, MAP_SHARED, map_fd, 0);
		ok &= expect(mapping != MAP_FAILED, "holds mapping to persistent config inode");
		auto refreshed = prepare(fixture, operations);
		ok &= expect(refreshed.has_value() && refreshed->runtime_dir == first->runtime_dir,
		             "unleased mapped slot updates in place");
		if (!refreshed.has_value()) {
			if (map_fd >= 0) {
				(void)close(map_fd);
			}
			if (mapping != MAP_FAILED) {
				(void)munmap(mapping, 10);
			}
			return false;
		}
		ok &= expect(inode_of(refreshed->config_path) == config_inode &&
		                 inode_of(refreshed->runtime_dir / kPreparedModelBackingFileName) ==
		                     backing_inode,
		             "refresh preserves actual backing inode bound");
		ok &= expect(read_file(refreshed->config_path) == "config-v2-longer\n",
		             "refresh writes new config content");
		std::array<char, 10> held_fd_content{};
		ok &= expect(map_fd >= 0 &&
		                 pread(map_fd, held_fd_content.data(), held_fd_content.size(), 0) ==
		                     static_cast<ssize_t>(held_fd_content.size()) &&
		                 std::string_view(held_fd_content.data(), held_fd_content.size()) ==
		                     "config-v2-",
		             "held fd observes in-place update");
		ok &= expect(mapping != MAP_FAILED &&
		                 std::string_view(static_cast<const char *>(mapping), 10) == "config-v2-",
		             "held mapping remains attached to updated inode");
		close_lease(refreshed);
		if (map_fd >= 0) {
			(void)close(map_fd);
		}
		if (mapping != MAP_FAILED) {
			(void)munmap(mapping, 10);
		}
		return ok;
	}

	auto expect_two_slot_limit(const Fixture                                   &fixture,
	                           const howdy::native::auth_helper::AclOperations &operations)
	    -> bool {
		bool ok = true;
		ok &= expect(write_file(fixture.config, "slot-a\n"), "writes slot A source");
		auto first = prepare(fixture, operations);
		ok &= expect(first.has_value(), "leases slot A");
		ok &= expect(write_file(fixture.config, "slot-b\n"), "writes slot B source");
		auto second = prepare(fixture, operations);
		ok &= expect(second.has_value() && first.has_value() &&
		                 second->runtime_dir != first->runtime_dir,
		             "active stale slot selects second fixed slot");
		ok &= expect(write_file(fixture.config, "slot-c\n"), "writes slot C source");
		auto blocked = prepare(fixture, operations);
		ok &= expect(!blocked.has_value(), "two active stale slots fail closed");

		const auto first_generation_inode =
		    first.has_value() ? inode_of(first->runtime_dir) : std::nullopt;
		const auto first_lock_inode =
		    first.has_value()
		        ? inode_of(
		              howdy::native::auth_helper_protocol::prepared_runtime_generation_lock_path(
		                  first->runtime_dir))
		        : std::nullopt;
		const auto first_models_inode =
		    first.has_value() ? inode_of(first->user_models_dir) : std::nullopt;
		const auto first_config_inode =
		    first.has_value() ? inode_of(first->config_path) : std::nullopt;
		const auto first_backing_inode =
		    first.has_value()
		        ? inode_of(first->runtime_dir /
		                   howdy::native::auth_helper_protocol::kPreparedModelBackingFileName)
		        : std::nullopt;
		close_lease(first);
		auto reused = prepare(fixture, operations);
		ok &= expect(
		    reused.has_value() && first_generation_inode.has_value() &&
		        inode_of(reused->runtime_dir) == first_generation_inode &&
		        inode_of(howdy::native::auth_helper_protocol::prepared_runtime_generation_lock_path(
		            reused->runtime_dir)) == first_lock_inode &&
		        inode_of(reused->user_models_dir) == first_models_inode &&
		        inode_of(reused->config_path) == first_config_inode &&
		        inode_of(reused->runtime_dir /
		                 howdy::native::auth_helper_protocol::kPreparedModelBackingFileName) ==
		            first_backing_inode,
		    "freed slot reuses same paths and actual inodes");
		close_lease(second);
		close_lease(reused);
		return ok;
	}

	auto expect_present_absent(const Fixture                                   &fixture,
	                           const howdy::native::auth_helper::AclOperations &operations)
	    -> bool {
		bool       ok           = true;
		const auto source_model = fixture.models / "alice.dat";
		ok &=
		    expect(write_file(source_model, "model-v1\n") && chmod(source_model.c_str(), 0644) == 0,
		           "creates secure source model");
		auto present = prepare(fixture, operations);
		ok &= expect(present.has_value(), "present model prepares");
		if (!present.has_value()) {
			return false;
		}
		const auto backing = present->runtime_dir /
		                     howdy::native::auth_helper_protocol::kPreparedModelBackingFileName;
		const auto visible = present->user_models_dir / "alice.dat";
		const auto backing_inode = inode_of(backing);
		ok &= expect(backing_inode.has_value() && inode_of(visible) == backing_inode,
		             "visible model is backing hard link");
		struct stat present_stat{};
		ok &= expect(lstat(backing.c_str(), &present_stat) == 0 && present_stat.st_nlink == 2,
		             "present backing has exactly two links");
		const int model_fd = open(visible.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		void     *mapping =
		    model_fd < 0 ? MAP_FAILED : mmap(nullptr, 9, PROT_READ, MAP_SHARED, model_fd, 0);
		ok &= expect(mapping != MAP_FAILED, "holds mapping to persistent model inode");
		close_lease(present);

		ok &= expect(write_file(source_model, "model-v2\n"), "updates source model");
		auto                refreshed = prepare(fixture, operations);
		std::array<char, 9> held_content{};
		ok &= expect(refreshed.has_value() && inode_of(backing) == backing_inode &&
		                 inode_of(visible) == backing_inode,
		             "model refresh preserves backing and visible inode identity");
		ok &= expect(model_fd >= 0 &&
		                 pread(model_fd, held_content.data(), held_content.size(), 0) ==
		                     static_cast<ssize_t>(held_content.size()) &&
		                 std::string_view(held_content.data(), held_content.size()) == "model-v2\n",
		             "held model fd observes in-place refresh");
		ok &= expect(mapping != MAP_FAILED &&
		                 std::string_view(static_cast<const char *>(mapping), 9) == "model-v2\n",
		             "held model mapping observes in-place refresh");
		close_lease(refreshed);

		std::error_code ec;
		std::filesystem::remove(source_model, ec);
		ok &= expect(!ec, "removes source model");
		auto        absent = prepare(fixture, operations);
		struct stat absent_stat{};
		struct stat held_absent_stat{};
		ok &= expect(absent.has_value() && !std::filesystem::exists(visible) &&
		                 inode_of(backing) == backing_inode &&
		                 lstat(backing.c_str(), &absent_stat) == 0 && absent_stat.st_nlink == 1 &&
		                 absent_stat.st_size == 0 && fstat(model_fd, &held_absent_stat) == 0 &&
		                 held_absent_stat.st_dev == absent_stat.st_dev &&
		                 held_absent_stat.st_ino == absent_stat.st_ino,
		             "absent cycle keeps empty persistent backing inode");
		close_lease(absent);

		ok &=
		    expect(write_file(source_model, "model-v3\n") && chmod(source_model.c_str(), 0644) == 0,
		           "restores source model");
		auto                restored = prepare(fixture, operations);
		std::array<char, 9> restored_held_content{};
		ok &= expect(restored.has_value() && inode_of(backing) == backing_inode &&
		                 inode_of(visible) == backing_inode && mapping != MAP_FAILED &&
		                 std::string_view(static_cast<const char *>(mapping), 9) == "model-v3\n" &&
		                 pread(model_fd, restored_held_content.data(), restored_held_content.size(),
		                       0) == static_cast<ssize_t>(restored_held_content.size()) &&
		                 std::string_view(restored_held_content.data(),
		                                  restored_held_content.size()) == "model-v3\n",
		             "present cycle restores link without replacing held backing inode");
		close_lease(restored);
		if (mapping != MAP_FAILED) {
			(void)munmap(mapping, 9);
		}
		if (model_fd >= 0) {
			(void)close(model_fd);
		}
		return ok;
	}

	auto expect_partial_slot_fallback(const std::filesystem::path &temp_root, uid_t target_uid,
	                                  const howdy::native::auth_helper::AclOperations &operations)
	    -> bool {
		using namespace howdy::native::auth_helper_protocol;
		bool ok      = true;
		auto fixture = make_fixture(temp_root, "partial-slot", target_uid);
		auto slot0   = fixture.has_value() ? prepare(*fixture, operations) : std::nullopt;
		ok &= expect(slot0.has_value(), "creates complete slot0 fixture");
		close_lease(slot0);
		if (!fixture.has_value() || !slot0.has_value()) {
			return false;
		}

		const auto      slot0_backing       = slot0->runtime_dir / kPreparedModelBackingFileName;
		const auto      slot0_backing_inode = inode_of(slot0_backing);
		const auto      slot0_models_inode  = inode_of(slot0->user_models_dir);
		std::error_code ec;
		std::filesystem::remove(slot0->config_path, ec);
		const auto expected_slot1 = prepared_runtime_generation_dir(fixture->root, target_uid,
		                                                            RuntimeGenerationSlot::kSlot1);
		auto       slot1          = prepare(*fixture, operations);
		ok &= expect(!ec && slot1.has_value() && slot1->runtime_dir == expected_slot1 &&
		                 !std::filesystem::exists(slot0->config_path) &&
		                 inode_of(slot0_backing) == slot0_backing_inode &&
		                 inode_of(slot0->user_models_dir) == slot0_models_inode,
		             "partial slot0 stays untouched while empty slot1 serves");
		if (!slot1.has_value()) {
			return false;
		}
		const auto slot1_config_inode = inode_of(slot1->config_path);
		const auto slot1_backing_inode =
		    inode_of(slot1->runtime_dir / kPreparedModelBackingFileName);
		auto repeated = prepare(*fixture, operations);
		ok &= expect(repeated.has_value() && repeated->runtime_dir == slot1->runtime_dir &&
		                 inode_of(repeated->config_path) == slot1_config_inode &&
		                 inode_of(repeated->runtime_dir / kPreparedModelBackingFileName) ==
		                     slot1_backing_inode,
		             "slot1 reuse stays on persistent bounded inodes");

		const auto slot0_lock = prepared_runtime_generation_lock_path(slot0->runtime_dir);
		const int  probe      = open(slot0_lock.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
		const bool lock_clean = probe >= 0 && flock(probe, LOCK_EX | LOCK_NB) == 0;
		ok &= expect(lock_clean, "abandoned malformed slot0 retains no helper lock");
		if (lock_clean) {
			(void)flock(probe, LOCK_UN);
		}
		if (probe >= 0) {
			(void)close(probe);
		}
		close_lease(slot1);
		close_lease(repeated);
		return ok;
	}

	auto
	expect_malformed_slot_discovery(const std::filesystem::path &temp_root, uid_t target_uid,
	                                const howdy::native::auth_helper::AclOperations &operations)
	    -> bool {
		using namespace howdy::native::auth_helper_protocol;
		bool ok = true;

		auto symlink_fixture = make_fixture(temp_root, "malformed-symlink", target_uid);
		if (symlink_fixture.has_value()) {
			std::error_code ec;
			std::filesystem::create_directories(symlink_fixture->root, ec);
			(void)chmod(symlink_fixture->root.c_str(), 0711);
			const auto slot0 = prepared_runtime_generation_dir(symlink_fixture->root, target_uid,
			                                                   RuntimeGenerationSlot::kSlot0);
			const auto slot1 = prepared_runtime_generation_dir(symlink_fixture->root, target_uid,
			                                                   RuntimeGenerationSlot::kSlot1);
			ok &= expect(symlink(symlink_fixture->source.c_str(), slot0.c_str()) == 0,
			             "creates slot symlink");
			const auto malformed_inode = inode_of(slot0);
			auto       fallback        = prepare(*symlink_fixture, operations);
			ok &= expect(fallback.has_value() && fallback->runtime_dir == slot1 &&
			                 inode_of(slot0) == malformed_inode,
			             "malformed generation directory remains while healthy slot serves");
			close_lease(fallback);
		}

		auto lock_fixture = make_fixture(temp_root, "malformed-lock", target_uid);
		auto lock_prepared =
		    lock_fixture.has_value() ? prepare(*lock_fixture, operations) : std::nullopt;
		ok &= expect(lock_prepared.has_value(), "creates lock fixture");
		close_lease(lock_prepared);
		if (lock_prepared.has_value()) {
			const auto lock_path =
			    prepared_runtime_generation_lock_path(lock_prepared->runtime_dir);
			const auto lock_inode = inode_of(lock_path);
			ok &= expect(chmod(lock_path.c_str(), 0644) == 0, "malforms lock mode");
			auto        fallback = prepare(*lock_fixture, operations);
			struct stat lock_stat{};
			ok &= expect(fallback.has_value() &&
			                 fallback->runtime_dir != lock_prepared->runtime_dir &&
			                 inode_of(lock_path) == lock_inode &&
			                 lstat(lock_path.c_str(), &lock_stat) == 0 &&
			                 (lock_stat.st_mode & 07777) == 0644,
			             "malformed lock remains while healthy slot serves");
			close_lease(fallback);
		}

		auto both_fixture = make_fixture(temp_root, "both-slots-malformed", target_uid);
		auto both_prepared =
		    both_fixture.has_value() ? prepare(*both_fixture, operations) : std::nullopt;
		ok &= expect(both_prepared.has_value(), "creates both-slot malformed fixture");
		close_lease(both_prepared);
		if (both_fixture.has_value() && both_prepared.has_value()) {
			const auto slot0_lock =
			    prepared_runtime_generation_lock_path(both_prepared->runtime_dir);
			const auto slot1      = prepared_runtime_generation_dir(both_fixture->root, target_uid,
			                                                        RuntimeGenerationSlot::kSlot1);
			const auto lock_inode = inode_of(slot0_lock);
			const auto slot_inode = inode_of(slot1);
			ok &= expect(chmod(slot0_lock.c_str(), 0644) == 0 && chmod(slot1.c_str(), 0700) == 0,
			             "malforms both fixed slots");
			auto        result = prepare(*both_fixture, operations);
			struct stat lock_stat{};
			struct stat slot_stat{};
			ok &= expect(
			    !result.has_value() && inode_of(slot0_lock) == lock_inode &&
			        inode_of(slot1) == slot_inode && lstat(slot0_lock.c_str(), &lock_stat) == 0 &&
			        lstat(slot1.c_str(), &slot_stat) == 0 && (lock_stat.st_mode & 07777) == 0644 &&
			        (slot_stat.st_mode & 07777) == 0700,
			    "two malformed slots fail closed without repair");
			const int  probe = open(slot0_lock.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
			const bool clean = probe >= 0 && flock(probe, LOCK_EX | LOCK_NB) == 0;
			ok &= expect(clean, "failed slot discovery leaves no lock held");
			if (clean) {
				(void)flock(probe, LOCK_UN);
			}
			if (probe >= 0) {
				(void)close(probe);
			}
		}
		return ok;
	}

	auto expect_malformed_objects_fail(const std::filesystem::path &temp_root, uid_t target_uid,
	                                   const howdy::native::auth_helper::AclOperations &operations)
	    -> bool {
		using namespace howdy::native::auth_helper_protocol;
		bool ok = true;

		auto mode_fixture = make_fixture(temp_root, "malformed-mode", target_uid);
		auto mode_prepared =
		    mode_fixture.has_value() ? prepare(*mode_fixture, operations) : std::nullopt;
		ok &= expect(mode_prepared.has_value(), "creates mode fixture");
		close_lease(mode_prepared);
		if (mode_prepared.has_value()) {
			ok &= expect(chmod(mode_prepared->config_path.c_str(), 0600) == 0,
			             "malforms config mode");
			auto        fallback = prepare(*mode_fixture, operations);
			struct stat damaged_stat{};
			ok &= expect(fallback.has_value() &&
			                 fallback->runtime_dir != mode_prepared->runtime_dir &&
			                 lstat(mode_prepared->config_path.c_str(), &damaged_stat) == 0 &&
			                 (damaged_stat.st_mode & 07777) == 0600,
			             "malformed config remains untouched while other slot serves");
			close_lease(fallback);
		}

		auto link_fixture = make_fixture(temp_root, "malformed-link", target_uid);
		auto link_prepared =
		    link_fixture.has_value() ? prepare(*link_fixture, operations) : std::nullopt;
		ok &= expect(link_prepared.has_value(), "creates link fixture");
		close_lease(link_prepared);
		if (link_prepared.has_value()) {
			const auto extra = temp_root / "unexpected-config-link";
			ok &= expect(link(link_prepared->config_path.c_str(), extra.c_str()) == 0,
			             "adds unexpected config hard link");
			auto        fallback = prepare(*link_fixture, operations);
			struct stat damaged_stat{};
			ok &= expect(fallback.has_value() &&
			                 fallback->runtime_dir != link_prepared->runtime_dir &&
			                 lstat(link_prepared->config_path.c_str(), &damaged_stat) == 0 &&
			                 damaged_stat.st_nlink == 2,
			             "unexpected link count remains untouched while other slot serves");
			close_lease(fallback);
		}

		auto visible_fixture = make_fixture(temp_root, "malformed-visible", target_uid);
		if (visible_fixture.has_value()) {
			const auto source_model = visible_fixture->models / "alice.dat";
			ok &= expect(write_file(source_model, "model\n") &&
			                 chmod(source_model.c_str(), 0644) == 0,
			             "creates visible identity source");
			auto visible_prepared = prepare(*visible_fixture, operations);
			ok &= expect(visible_prepared.has_value(), "creates visible identity fixture");
			close_lease(visible_prepared);
			if (visible_prepared.has_value()) {
				const auto      visible = visible_prepared->user_models_dir / "alice.dat";
				std::error_code ec;
				std::filesystem::remove(visible, ec);
				ok &= expect(!ec && write_file(visible, "foreign inode\n"),
				             "replaces visible name with foreign inode");
				const auto foreign_inode = inode_of(visible);
				auto       fallback      = prepare(*visible_fixture, operations);
				ok &= expect(fallback.has_value() &&
				                 fallback->runtime_dir != visible_prepared->runtime_dir &&
				                 inode_of(visible) == foreign_inode,
				             "visible mismatch remains untouched while other slot serves");
				close_lease(fallback);
			}
		}

		if (geteuid() == 0) {
			auto owner_fixture = make_fixture(temp_root, "malformed-owner", target_uid);
			auto owner_prepared =
			    owner_fixture.has_value() ? prepare(*owner_fixture, operations) : std::nullopt;
			ok &= expect(owner_prepared.has_value(), "creates owner fixture");
			close_lease(owner_prepared);
			if (owner_prepared.has_value()) {
				ok &= expect(chown(owner_prepared->config_path.c_str(), 61007, getegid()) == 0,
				             "malforms config owner");
				auto        fallback = prepare(*owner_fixture, operations);
				struct stat damaged_stat{};
				ok &= expect(fallback.has_value() &&
				                 fallback->runtime_dir != owner_prepared->runtime_dir &&
				                 lstat(owner_prepared->config_path.c_str(), &damaged_stat) == 0 &&
				                 damaged_stat.st_uid == 61007,
				             "malformed owner remains untouched while other slot serves");
				close_lease(fallback);
			}
		}
		return ok;
	}

}  // namespace

auto run_auth_helper_staging_tests(const std::filesystem::path    &temp_root,
                                   const AuthHelperStagingContext &context) -> bool {
	bool ok = expect_protocol_paths();
	if (!context.acl_functional) {
		std::cerr << "SKIP: fixed-slot tests require functional ACL operations\n";
		return ok;
	}
	const uid_t target_uid = context.functional_target_uid;
	for (const auto &[name, test] : {
	         std::pair{"model", expect_present_absent},
	         std::pair{"reuse", expect_reuse_and_inode_bound},
	         std::pair{"limit", expect_two_slot_limit},
	     }) {
		auto fixture = make_fixture(temp_root, name, target_uid);
		ok &= expect(fixture.has_value(), std::string("creates ") + name + " fixture");
		if (fixture.has_value()) {
			ok &= test(*fixture, context.operations);
		}
	}
	ok &= expect_partial_slot_fallback(temp_root, target_uid, context.operations);
	ok &= expect_malformed_slot_discovery(temp_root, target_uid, context.operations);
	ok &= expect_malformed_objects_fail(temp_root, target_uid, context.operations);
	if (context.acl_supported) {
		ok &= expect_cross_uid_acl_access(temp_root, context.production_operations);
	}
	return ok;
}
