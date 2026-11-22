#include "auth_helper/auth_helper_test_groups.hpp"
#include "auth_helper/runtime_descriptors.hpp"
#include "protocol/runtime_descriptor_message.hpp"
#include "storage/user_model_store.hpp"
#include "support/bit_flags.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>

namespace {

	namespace fs = std::filesystem;
	using howdy::native::auth_helper::OpenedRuntimeDescriptors;
	using howdy::native::auth_helper::OpenRuntimeDescriptors;
	using howdy::native::auth_helper::RuntimeDescriptorSources;
	using howdy::native::runtime_descriptor_message::ModelState;
	using howdy::test::Expect;
	using howdy::test::WriteFile;

	// Whether `fd` is a read-only, close-on-exec descriptor for the file at `path`.
	auto NamesFile(int fd, const fs::path &path) -> bool {
		struct stat opened{};
		struct stat named{};
		const int   flags      = fcntl(fd, F_GETFL);
		const int   descriptor = fcntl(fd, F_GETFD);
		return flags >= 0 &&
		       howdy::native::MaskedBits(flags, O_ACCMODE) ==
		           howdy::native::BitRepresentation(O_RDONLY) &&
		       descriptor >= 0 && howdy::native::HasAnyBits(descriptor, FD_CLOEXEC) &&
		       fstat(fd, &opened) == 0 && stat(path.c_str(), &named) == 0 &&
		       opened.st_dev == named.st_dev && opened.st_ino == named.st_ino;
	}

}  // namespace

auto RunAuthHelperDescriptorTests(const fs::path &temp_root) -> bool {
	bool            ok   = true;
	const auto      root = temp_root / "descriptors";
	std::error_code ec;
	fs::remove_all(root, ec);
	fs::create_directories(root / "models", ec);
	ok &=
	    Expect(!ec && chmod(root.c_str(), 0755) == 0 && chmod((root / "models").c_str(), 0755) == 0,
		       "create descriptor opener fixture");
	const auto config = root / "config.ini";
	const auto model  = root / "models" / "alice.dat";
	ok &=
	    Expect(WriteFile(config, "[core]\ndisabled = false\n") && chmod(config.c_str(), 0644) == 0,
		       "write descriptor opener config");
	ok &= Expect(WriteFile(model, "[]") && chmod(model.c_str(), 0600) == 0,
	             "write descriptor opener model");

	const RuntimeDescriptorSources sources{
	    .config                 = config,
	    .user_models            = howdy::native::UserModelStore({
	        .models_dir      = root / "models",
	        .owner_uid       = geteuid(),
	        .validation_root = {.path = root},
	    }),
	    .config_owner_uid       = geteuid(),
	    .config_validation_root = {.path = root},
	};
	const auto open_for = [&](const std::string &user) -> OpenedRuntimeDescriptors {
		return OpenRuntimeDescriptors(user, sources);
	};

	{
		const auto opened = open_for("alice");
		ok &= Expect(opened.ok && opened.model_state == ModelState::kReady &&
		                 opened.model_message.empty(),
		             "opener reports a ready model");
		ok &= Expect(NamesFile(opened.config.Get(), config) && NamesFile(opened.model.Get(), model),
		             "opener descriptors are read-only, close-on-exec and name the files");
	}
	{
		const auto opened = open_for("bob");
		ok &= Expect(opened.ok && opened.model_state == ModelState::kMissing &&
		                 !opened.model.Valid() && opened.model_message.empty() &&
		                 NamesFile(opened.config.Get(), config),
		             "opener reports a missing model and still opens the config");
	}
	{
		const auto opened = open_for("../alice");
		ok &= Expect(opened.ok && opened.model_state == ModelState::kInvalidUser &&
		                 !opened.model.Valid() && !opened.model_message.empty(),
		             "opener reports an invalid user name");
	}

	ok &= Expect(chmod(model.c_str(), 0666) == 0, "make opener model world-writable");
	{
		const auto opened = open_for("alice");
		ok &= Expect(opened.ok && opened.model_state == ModelState::kInvalidStorage &&
		                 !opened.model.Valid() && !opened.model_message.empty(),
		             "opener rejects a writable model");
	}
	ok &= Expect(chmod(model.c_str(), 0600) == 0, "restore opener model mode");

	const auto link_path = root / "models" / "alice-link.dat";
	ok &= Expect(link(model.c_str(), link_path.c_str()) == 0, "hard-link opener model");
	{
		const auto opened = open_for("alice");
		ok &= Expect(opened.ok && opened.model_state == ModelState::kInvalidStorage &&
		                 !opened.model.Valid(),
		             "opener rejects a hard-linked model");
	}
	ok &= Expect(unlink(link_path.c_str()) == 0, "remove opener model hard link");

	// No writer produces an empty model; one is left over, for example by an older version.
	const auto stranded = root / "models" / "carol.dat";
	ok &= Expect(WriteFile(stranded, "") && chmod(stranded.c_str(), 0600) == 0,
	             "create an empty model, as an older version could leave behind");
	{
		const auto opened = open_for("carol");
		ok &= Expect(opened.ok && opened.model_state == ModelState::kInvalidStorage &&
		                 !opened.model.Valid() && opened.model_message.contains("empty") &&
		                 opened.model_message.contains(stranded.string()),
		             "opener reports an empty model as invalid storage, by name");
	}

	{
		auto other_model_owner        = sources;
		other_model_owner.user_models = howdy::native::UserModelStore({
		    .models_dir      = root / "models",
		    .owner_uid       = static_cast<uid_t>(geteuid() + 1),
		    .validation_root = {.path = root},
		});
		const auto opened             = OpenRuntimeDescriptors("alice", other_model_owner);
		ok &= Expect(opened.ok && opened.model_state == ModelState::kInvalidStorage &&
		                 !opened.model.Valid() && opened.model_message.contains("owned by UID"),
		             "opener rejects a model with another owner");
	}

	fs::remove_all(root / "models", ec);
	{
		const auto opened = open_for("alice");
		ok &= Expect(opened.ok && opened.model_state == ModelState::kMissing,
		             "opener reports a missing model directory as a missing model");
	}

	{
		auto other_owner             = sources;
		other_owner.config_owner_uid = geteuid() + 1;
		const auto opened            = OpenRuntimeDescriptors("alice", other_owner);
		ok &= Expect(!opened.ok && !opened.config.Valid() &&
		                 opened.error_message.contains("owned by UID"),
		             "opener rejects a config with another owner");
	}
	ok &= Expect(chmod(config.c_str(), 0666) == 0, "make opener config world-writable");
	{
		const auto opened = open_for("alice");
		ok &= Expect(!opened.ok && !opened.config.Valid() && !opened.error_message.empty(),
		             "opener rejects a writable config");
	}
	ok &= Expect(chmod(config.c_str(), 0644) == 0, "restore opener config mode");

	const auto linked_config = root / "linked.ini";
	ok &= Expect(symlink(config.c_str(), linked_config.c_str()) == 0, "symlink opener config");
	{
		auto symlinked    = sources;
		symlinked.config  = linked_config;
		const auto opened = OpenRuntimeDescriptors("alice", symlinked);
		ok &= Expect(!opened.ok && opened.error_message.contains(linked_config.string()),
		             "opener refuses to follow a symlinked config");
	}
	{
		auto missing      = sources;
		missing.config    = root / "missing.ini";
		const auto opened = OpenRuntimeDescriptors("alice", missing);
		ok &= Expect(!opened.ok && opened.error_message.contains("missing.ini"),
		             "opener reports a missing config");
	}

	fs::remove_all(root, ec);
	return ok;
}
