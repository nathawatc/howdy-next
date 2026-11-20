#include "protocol/runtime_descriptor_message.hpp"
#include "runtime/runtime_session.hpp"
#include "storage/user_model_status.hpp"
#include "storage/user_models.hpp"

#include <cstddef>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <unistd.h>

#include <sys/stat.h>

// Runs the production runtime session for the installed module's paths: as root it opens the
// files in-process, as any other user it goes through the installed setuid helper's `open`.
// Usage: --open <user> <config> <models-directory> ready|missing|invalid
namespace {

	namespace message = howdy::native::runtime_descriptor_message;

	auto Fail(const std::string &reason) -> int {
		std::cerr << "Installed runtime driver: " << reason << '\n';
		return 1;
	}

	// What the session hands compare: close-on-exec, read-only, a regular root-owned file.
	auto DescriptorUsable(int fd) -> bool {
		struct stat info{};
		const int   descriptor_flags = fcntl(fd, F_GETFD);
		const int   status_flags     = fcntl(fd, F_GETFL);
		return descriptor_flags >= 0 && (descriptor_flags & FD_CLOEXEC) != 0 && status_flags >= 0 &&
		       (status_flags & O_ACCMODE) == O_RDONLY && fstat(fd, &info) == 0 &&
		       S_ISREG(info.st_mode) && info.st_uid == 0;
	}

	auto ExpectedState(std::string_view name, message::ModelState *state) -> bool {
		if (name == "ready") {
			*state = message::ModelState::kReady;
		} else if (name == "missing") {
			*state = message::ModelState::kMissing;
		} else if (name == "invalid") {
			*state = message::ModelState::kInvalidStorage;
		} else {
			return false;
		}
		return true;
	}

}  // namespace

auto main(int argc, char **argv) -> int {
	const auto arguments =
	    std::span(argv, argc > 0 ? static_cast<std::size_t>(argc) : std::size_t{0});
	message::ModelState expected = message::ModelState::kInvalidStorage;
	if (arguments.size() != 6 || std::string_view(arguments.subspan(1).front()) != "--open" ||
	    !ExpectedState(arguments.back(), &expected)) {
		return Fail("usage: --open <user> <config> <models-directory> ready|missing|invalid");
	}
	const std::string user(arguments.subspan(2).front());
	const std::string config(arguments.subspan(3).front());
	const std::string models(arguments.subspan(4).front());

	howdy::pam::RuntimeSession session({.config_path = config, .user_models_dir = models}, 0,
	                                   howdy::pam::ProductionRuntimeSessionOperations());
	const auto                 result = session.LoadForUser(user);
	if (!howdy::pam::RuntimeSessionLoadSucceeded(result)) {
		return Fail("runtime session failed: " + result.error_message +
		            result.config_result.error_message);
	}
	if (session.ModelState() != expected) {
		return Fail("unexpected model state");
	}
	if (!DescriptorUsable(session.ConfigFd())) {
		return Fail("config descriptor is not usable");
	}
	if (expected == message::ModelState::kReady) {
		if (!DescriptorUsable(session.ModelFd())) {
			return Fail("model descriptor is not usable");
		}
		// The model reads back through the same loader compare uses.
		const auto models_loaded = howdy::native::LoadUserModelsFromFd(
		    session.ModelFd(), "model descriptor", "opencv_dnn_sface", 0);
		if (models_loaded.status != howdy::native::UserModelStatus::kOk ||
		    models_loaded.stored.encodings.empty()) {
			return Fail("model descriptor does not load: " + models_loaded.error_message);
		}
	} else if (session.ModelFd() >= 0) {
		return Fail("a model descriptor came with a model that is not ready");
	}
	std::cout << "HOWDY_E2E_RUNTIME_OK\n";
	return 0;
}
