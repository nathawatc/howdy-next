#include "storage/user_model_limits.hpp"
#include "storage/user_model_status.hpp"
#include "storage/user_model_types.hpp"
#include "storage/user_models.hpp"
#include "storage/user_models_test_support.hpp"
#include "test_support.hpp"

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>

namespace howdy::test::user_models {

	namespace {

		constexpr auto kBackend = "opencv_dnn_sface";
		constexpr auto kTwoModels =
		    R"([{"id":1,"time":1,"label":"first","backend":"opencv_dnn_sface","data":[[0.1,0.2]]},)"
		    R"({"id":2,"time":2,"label":"second","backend":"opencv_dnn_sface","data":[[0.3,0.4],[0.5,0.6]]}])";

		auto WriteSecureModel(const std::filesystem::path &path, const std::string &content)
		    -> bool {
			return WriteFile(path, content) && chmod(path.c_str(), 0600) == 0;
		}

		auto OpenReadOnly(const std::filesystem::path &path) -> howdy::test::ScopedFd {
			return howdy::test::ScopedFd(open(path.c_str(), O_RDONLY | O_CLOEXEC));
		}

		auto ExpectStatus(const howdy::native::UserModelLoadResult &result,
		                  howdy::native::UserModelStatus status, const std::string &message)
		    -> bool {
			bool ok = Expect(result.status == status, message + " status");
			if (status != howdy::native::UserModelStatus::kOk &&
			    status != howdy::native::UserModelStatus::kNoModel) {
				ok &= Expect(!result.error_message.empty(), message + " has an error message");
			}
			ok &= Expect(status == howdy::native::UserModelStatus::kOk ||
			                 result.stored.encodings.empty(),
			             message + " has no encodings");
			return ok;
		}

	}  // namespace

	// Descriptors stand in for ones another process opened: no path is walked or opened, so only
	// the descriptor itself decides, and the shared file offset must not move.
	auto TestUserModelDescriptorLoading() -> bool {
		namespace fs = std::filesystem;
		using howdy::native::LoadUserModelsFromFd;
		using howdy::native::UserModelStatus;

		bool            ok        = true;
		const auto      temp_root = fs::current_path() / "howdy-user-models-fd-test";
		std::error_code ec;
		fs::remove_all(temp_root, ec);
		fs::create_directories(temp_root, ec);
		ok &= Expect(!ec, "create descriptor model temp root");

		const uid_t    owner        = geteuid();
		const fs::path display_path = "/etc/howdy/models/alice.dat";
		const auto     model_path   = temp_root / "alice.dat";
		ok &= Expect(WriteSecureModel(model_path, kTwoModels), "write descriptor model");

		{
			const auto model_fd = OpenReadOnly(model_path);
			ok &= Expect(model_fd.Valid() && lseek(model_fd.Get(), 7, SEEK_SET) == 7,
			             "open descriptor model away from offset 0");
			const auto result = LoadUserModelsFromFd(model_fd.Get(), display_path, kBackend, owner);
			ok &= ExpectStatus(result, UserModelStatus::kOk, "descriptor model loads");
			ok &= Expect(result.stored.encodings.size() == 3 && result.stored.models.size() == 3 &&
			                 result.stored.models.at(0).id == 1 &&
			                 result.stored.models.at(2).label == "second",
			             "descriptor model flattens every encoding");
			ok &= Expect(lseek(model_fd.Get(), 0, SEEK_CUR) == 7,
			             "descriptor model load leaves the shared offset alone");

			const auto wrong_owner = LoadUserModelsFromFd(model_fd.Get(), display_path, kBackend,
			                                              static_cast<uid_t>(owner + 1));
			ok &= ExpectStatus(wrong_owner, UserModelStatus::kInsecurePath,
			                   "descriptor model with another owner");
			ok &= Expect(wrong_owner.error_message.contains(display_path.string()),
			             "descriptor model error names the display path");

			ok &= Expect(fchmod(model_fd.Get(), 0620) == 0, "make descriptor model group-writable");
			ok &= ExpectStatus(LoadUserModelsFromFd(model_fd.Get(), display_path, kBackend, owner),
			                   UserModelStatus::kInsecurePath, "group-writable descriptor model");
			ok &= Expect(fchmod(model_fd.Get(), 0602) == 0, "make descriptor model world-writable");
			ok &= ExpectStatus(LoadUserModelsFromFd(model_fd.Get(), display_path, kBackend, owner),
			                   UserModelStatus::kInsecurePath, "world-writable descriptor model");
			ok &= Expect(fchmod(model_fd.Get(), 0600) == 0, "restore descriptor model mode");
		}

		{
			// A second link is never accepted, even under a staged runtime path: descriptors come
			// from the canonical store.
			const auto linked_path = temp_root / "alice-link.dat";
			ok &= Expect(link(model_path.c_str(), linked_path.c_str()) == 0,
			             "hard-link descriptor model");
			const auto model_fd = OpenReadOnly(model_path);
			ok &= ExpectStatus(LoadUserModelsFromFd(model_fd.Get(),
			                                        "/run/howdy/pam-1000-gen000/models/alice.dat",
			                                        kBackend, owner),
			                   UserModelStatus::kInsecurePath, "hard-linked descriptor model");
			ok &= Expect(unlink(linked_path.c_str()) == 0, "remove descriptor model hard link");
		}

		{
			const howdy::test::ScopedFd read_write(open(model_path.c_str(), O_RDWR | O_CLOEXEC));
			const auto                  result =
			    LoadUserModelsFromFd(read_write.Get(), display_path, kBackend, owner);
			ok &= ExpectStatus(result, UserModelStatus::kInsecurePath, "writable descriptor model");
			ok &= Expect(result.error_message.contains("must be open read-only"),
			             "writable descriptor model reports its open mode");

			const howdy::test::ScopedFd path_only(open(model_path.c_str(), O_PATH | O_CLOEXEC));
			ok &= ExpectStatus(LoadUserModelsFromFd(path_only.Get(), display_path, kBackend, owner),
			                   UserModelStatus::kInsecurePath, "O_PATH descriptor model");

			ok &= ExpectStatus(LoadUserModelsFromFd(-1, display_path, kBackend, owner),
			                   UserModelStatus::kInsecurePath, "negative descriptor model");

			const howdy::test::ScopedFd directory(
			    open(temp_root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
			ok &= ExpectStatus(LoadUserModelsFromFd(directory.Get(), display_path, kBackend, owner),
			                   UserModelStatus::kInsecurePath, "directory descriptor model");

			const auto fifo_path = temp_root / "alice.fifo";
			ok &= Expect(mkfifo(fifo_path.c_str(), 0600) == 0, "create descriptor model FIFO");
			const howdy::test::ScopedFd fifo(
			    open(fifo_path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
			ok &= ExpectStatus(LoadUserModelsFromFd(fifo.Get(), display_path, kBackend, owner),
			                   UserModelStatus::kInsecurePath, "FIFO descriptor model");
		}

		const auto load_content =
		    [&](const std::string &label,
			    const std::string &content) -> howdy::native::UserModelLoadResult {
			const auto path = temp_root / (label + ".dat");
			ok &= Expect(WriteSecureModel(path, content), "write " + label + " descriptor model");
			const auto model_fd = OpenReadOnly(path);
			return LoadUserModelsFromFd(model_fd.Get(), display_path, kBackend, owner);
		};

		ok &= ExpectStatus(
		    load_content(
		        "oversized",
		        std::string(howdy::native::user_model_limits::kMaxUserModelFileBytes + 1, ' ')),
		    UserModelStatus::kParseError, "oversized descriptor model");
		ok &= ExpectStatus(load_content("malformed", "[{"), UserModelStatus::kParseError,
		                   "malformed descriptor model");
		ok &= ExpectStatus(load_content("empty-file", ""), UserModelStatus::kParseError,
		                   "empty descriptor model file");
		ok &= ExpectStatus(load_content("empty-list", "[]"), UserModelStatus::kNoModel,
		                   "empty descriptor model list");
		ok &= ExpectStatus(
		    load_content(
		        "incompatible",
		        R"([{"id":1,"label":"bad","backend":"other_backend","data":[[0.1,0.2]]}])"),
		    UserModelStatus::kIncompatibleBackend, "incompatible descriptor model backend");

		{
			// Once the file is unlinked (replaced or removed) it has no link left, and the load
			// fails closed instead of using the stale contents.
			const auto model_fd = OpenReadOnly(model_path);
			ok &= Expect(unlink(model_path.c_str()) == 0, "unlink descriptor model");
			ok &= ExpectStatus(LoadUserModelsFromFd(model_fd.Get(), display_path, kBackend, owner),
			                   UserModelStatus::kInsecurePath, "unlinked descriptor model");
		}

		fs::remove_all(temp_root, ec);
		return ok;
	}

}  // namespace howdy::test::user_models
