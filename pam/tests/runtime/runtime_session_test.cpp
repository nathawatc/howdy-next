#include "config/runtime_config.hpp"
#include "config/runtime_config_loader.hpp"
#include "protocol/runtime_descriptor_message.hpp"
#include "runtime/runtime_session.hpp"
#include "runtime/runtime_session_test_groups.hpp"
#include "support/scoped_fd.hpp"
#include "test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>  // IWYU pragma: keep

namespace {

	namespace message = howdy::native::runtime_descriptor_message;
	using howdy::native::ScopedFd;
	using howdy::pam::RuntimeDescriptorSet;
	using howdy::pam::RuntimeSession;
	using howdy::pam::RuntimeSessionLoadStatus;
	using howdy::pam::RuntimeSessionOperations;
	using howdy::pam::RuntimeSessionPaths;
	using howdy::test::Expect;
	using message::ModelState;

	constexpr auto kConfiguredConfig = "/etc/howdy/config.ini";
	constexpr auto kConfiguredModels = "/var/lib/howdy/models";

	// What the fake opener hands out: descriptors for real files, which the session checks.
	struct FakeContext {
		std::filesystem::path config_file;
		std::filesystem::path model_file;
		uid_t                 effective_uid  = 1000;
		bool                  open_succeeds  = true;
		ModelState            model_state    = ModelState::kReady;
		int                   config_flags   = O_RDONLY | O_CLOEXEC;
		bool                  send_model     = true;
		bool                  config_loads   = true;
		int                   in_process     = 0;
		int                   through_helper = 0;
		int                   load_calls     = 0;
		int                   loaded_fd      = -1;
		uid_t                 loaded_owner   = 0;
		RuntimeSessionPaths   seen_paths;
		std::filesystem::path loaded_display;
	};

	auto Fill(FakeContext &fake, RuntimeDescriptorSet *descriptors) -> bool {
		if (!fake.open_succeeds) {
			return false;
		}
		descriptors->config      = ScopedFd(open(fake.config_file.c_str(), fake.config_flags));
		descriptors->model_state = fake.model_state;
		if (fake.send_model) {
			descriptors->model = ScopedFd(open(fake.model_file.c_str(), O_RDONLY | O_CLOEXEC));
		}
		return true;
	}

	auto OpenInProcess(void *context, std::string_view username, const RuntimeSessionPaths &paths,
	                   RuntimeDescriptorSet *descriptors) -> bool {
		(void)username;
		auto &fake = *static_cast<FakeContext *>(context);
		++fake.in_process;
		fake.seen_paths = paths;
		return Fill(fake, descriptors);
	}

	auto OpenThroughHelper(void *context, std::string_view username,
	                       RuntimeDescriptorSet *descriptors) -> bool {
		(void)username;
		auto &fake = *static_cast<FakeContext *>(context);
		++fake.through_helper;
		return Fill(fake, descriptors);
	}

	auto LoadConfig(void *context, int config_fd, const std::filesystem::path &display_path,
	                uid_t owner_uid) -> howdy::native::RuntimeConfigLoadResult {
		auto &fake = *static_cast<FakeContext *>(context);
		++fake.load_calls;
		fake.loaded_fd      = config_fd;
		fake.loaded_display = display_path;
		fake.loaded_owner   = owner_uid;
		if (!fake.config_loads) {
			return {
			    .ok            = false,
			    .status        = howdy::native::RuntimeConfigLoadStatus::kParseError,
			    .path          = display_path,
			    .error_message = "parse failure",
			};
		}
		return {
		    .ok     = true,
		    .status = howdy::native::RuntimeConfigLoadStatus::kOk,
		    .path   = display_path,
		    .config = howdy::native::RuntimeConfig{},
		};
	}

	auto EffectiveUid(void *context) -> uid_t {
		return static_cast<const FakeContext *>(context)->effective_uid;
	}

	auto Operations(FakeContext *context) -> RuntimeSessionOperations {
		auto operations = RuntimeSessionOperations::Create(
		    context, OpenInProcess, OpenThroughHelper, LoadConfig, EffectiveUid);
		if (!operations.has_value()) {
			std::abort();
		}
		return *operations;
	}

	class Fixture {
	public:
		Fixture() {
			std::error_code error;
			std::filesystem::create_directories(root_, error);
			fake_.config_file = root_ / "config.ini";
			fake_.model_file  = root_ / "alice.dat";
			(void)howdy::test::WriteFile(fake_.config_file, "[core]\n");
			(void)howdy::test::WriteFile(fake_.model_file, "[]");
			(void)chmod(fake_.config_file.c_str(), 0644);
			(void)chmod(fake_.model_file.c_str(), 0600);
		}

		Fixture(const Fixture &)                     = delete;
		auto operator=(const Fixture &) -> Fixture & = delete;
		Fixture(Fixture &&)                          = delete;
		auto operator=(Fixture &&) -> Fixture &      = delete;

		~Fixture() {
			std::error_code error;
			std::filesystem::remove_all(root_, error);
		}

		[[nodiscard]] auto Fake() -> FakeContext & {
			return fake_;
		}

		auto Session(uid_t owner) -> RuntimeSession {
			return {{.config_path = kConfiguredConfig, .user_models_dir = kConfiguredModels},
			        owner,
			        Operations(&fake_)};
		}

	private:
		std::filesystem::path root_ = std::filesystem::current_path() /
		                              ("howdy-runtime-session-test-" + std::to_string(getpid()));
		FakeContext           fake_;
	};

	auto TestHelperForNonRoot() -> bool {
		Fixture    fixture;
		auto       session = fixture.Session(geteuid());
		const auto result  = session.LoadForUser("alice");
		return Expect(result.status == RuntimeSessionLoadStatus::kOk &&
		                  howdy::pam::RuntimeSessionLoadSucceeded(result),
		              "a non-root session loads through the helper") &&
		       Expect(fixture.Fake().through_helper == 1 && fixture.Fake().in_process == 0,
		              "a non-root session asks the helper, not the in-process opener") &&
		       Expect(fixture.Fake().load_calls == 1 &&
		                  fixture.Fake().loaded_fd == session.ConfigFd() &&
		                  fixture.Fake().loaded_display == kConfiguredConfig &&
		                  fixture.Fake().loaded_owner == geteuid(),
		              "the config is loaded from its descriptor, named by the configured path") &&
		       Expect(session.ModelState() == ModelState::kReady && session.ConfigFd() >= 0 &&
		                  session.ModelFd() >= 0,
		              "the session keeps the ready model and both descriptors");
	}

	auto TestInProcessForRoot() -> bool {
		Fixture fixture;
		fixture.Fake().effective_uid = 0;
		auto       session           = fixture.Session(geteuid());
		const auto result            = session.LoadForUser("alice");
		return Expect(result.status == RuntimeSessionLoadStatus::kOk,
		              "a root session loads in-process") &&
		       Expect(fixture.Fake().in_process == 1 && fixture.Fake().through_helper == 0,
		              "a root session opens the files itself, without the helper") &&
		       Expect(fixture.Fake().seen_paths.config_path == kConfiguredConfig &&
		                  fixture.Fake().seen_paths.user_models_dir == kConfiguredModels,
		              "the in-process opener gets the configured paths");
	}

	auto TestMissingModelStillLoadsConfig() -> bool {
		Fixture fixture;
		fixture.Fake().model_state = ModelState::kMissing;
		fixture.Fake().send_model  = false;
		auto       session         = fixture.Session(geteuid());
		const auto result          = session.LoadForUser("alice");
		return Expect(result.status == RuntimeSessionLoadStatus::kOk &&
		                  session.ModelState() == ModelState::kMissing && session.ModelFd() < 0 &&
		                  session.ConfigFd() >= 0,
		              "a missing model still loads the config, so eligibility can decide");
	}

	auto TestOpenFailure() -> bool {
		Fixture fixture;
		fixture.Fake().open_succeeds = false;
		auto       session           = fixture.Session(geteuid());
		const auto result            = session.LoadForUser("alice");
		return Expect(result.status == RuntimeSessionLoadStatus::kOpenFailed &&
		                  fixture.Fake().load_calls == 0 && session.ConfigFd() < 0,
		              "an open failure loads nothing and keeps no descriptor");
	}

	auto TestConfigFailure() -> bool {
		Fixture fixture;
		fixture.Fake().config_loads = false;
		auto       session          = fixture.Session(geteuid());
		const auto result           = session.LoadForUser("alice");
		return Expect(result.status == RuntimeSessionLoadStatus::kConfigLoadFailed &&
		                  result.config_result.error_message == "parse failure" &&
		                  session.ConfigFd() < 0 && session.ModelFd() < 0,
		              "a config failure keeps no descriptor");
	}

	// Each case breaks one thing the session must check before trusting the descriptors.
	auto TestDescriptorValidation() -> bool {
		bool       ok      = true;
		const auto rejects = [&](auto configure, uid_t owner_offset,
		                         std::string_view label) -> void {
			Fixture fixture;
			configure(fixture);
			auto       session = fixture.Session(static_cast<uid_t>(geteuid() + owner_offset));
			const auto result  = session.LoadForUser("alice");
			ok &= Expect(result.status == RuntimeSessionLoadStatus::kOpenFailed &&
			                 !result.error_message.empty() && fixture.Fake().load_calls == 0 &&
			                 session.ConfigFd() < 0,
			             std::string(label) + " is rejected before the config loads");
		};
		rejects(
		    [](Fixture &fixture) -> void {
			    fixture.Fake().config_file = "/nonexistent/config";
		    },
		    0, "a missing config descriptor");
		rejects(
		    [](Fixture &fixture) -> void {
			    fixture.Fake().send_model = false;
		    },
		    0, "a ready model without a descriptor");
		rejects(
		    [](Fixture &fixture) -> void {
			    fixture.Fake().model_state = ModelState::kMissing;
		    },
		    0, "a model descriptor with a model that is not ready");
		rejects(
		    [](Fixture &fixture) -> void {
			    fixture.Fake().config_flags = O_RDONLY;
		    },
		    0, "a config descriptor that is not close-on-exec");
		rejects(
		    [](Fixture &fixture) -> void {
			    fixture.Fake().config_flags = O_RDWR | O_CLOEXEC;
		    },
		    0, "a writable config descriptor");
		rejects(
		    [](Fixture &fixture) -> void {
			    (void)chmod(fixture.Fake().model_file.c_str(), 0666);
		    },
		    0, "a world-writable model");
		rejects([](Fixture &) -> void {}, 1, "descriptors with another owner");
		return ok;
	}

	auto TestOneShot() -> bool {
		Fixture fixture;
		auto    session = fixture.Session(geteuid());
		(void)session.LoadForUser("alice");
		const int  config_fd = session.ConfigFd();
		const auto second    = session.LoadForUser("alice");
		bool ok = Expect(second.status == RuntimeSessionLoadStatus::kAlreadyLoaded &&
		                     fixture.Fake().through_helper == 1 && session.ConfigFd() == config_fd,
		                 "a second load is rejected and keeps the first result");

		Fixture failing;
		failing.Fake().open_succeeds = false;
		auto failed_session          = failing.Session(geteuid());
		(void)failed_session.LoadForUser("alice");
		ok &= Expect(failed_session.LoadForUser("alice").status ==
		                     RuntimeSessionLoadStatus::kAlreadyLoaded &&
		                 failing.Fake().through_helper == 1,
		             "a failed load is not retried");
		return ok;
	}

	auto TestMissingDependencies() -> bool {
		return Expect(!RuntimeSessionOperations::Create(nullptr, nullptr, OpenThroughHelper,
		                                                LoadConfig, EffectiveUid)
		                      .has_value() &&
		                  !RuntimeSessionOperations::Create(nullptr, OpenInProcess, nullptr,
		                                                    LoadConfig, EffectiveUid)
		                       .has_value() &&
		                  !RuntimeSessionOperations::Create(
		                       nullptr, OpenInProcess, OpenThroughHelper, nullptr, EffectiveUid)
		                       .has_value() &&
		                  !RuntimeSessionOperations::Create(nullptr, OpenInProcess,
		                                                    OpenThroughHelper, LoadConfig, nullptr)
		                       .has_value(),
		              "runtime session operations require every callback");
	}

}  // namespace

namespace {

	// Sends a message SendMessage would never build: a wrong marker.
	auto SendWrongMarker(int config_fd) -> bool {
		std::array<char, 2> payload{'X', static_cast<char>(ModelState::kMissing)};
		iovec               vector{.iov_base = payload.data(), .iov_len = payload.size()};
		alignas(cmsghdr) std::array<std::byte, CMSG_SPACE(sizeof(int))> control{};
		msghdr                                                          outgoing{};
		outgoing.msg_iov        = &vector;
		outgoing.msg_iovlen     = 1;
		outgoing.msg_control    = control.data();
		outgoing.msg_controllen = control.size();
		cmsghdr *header         = CMSG_FIRSTHDR(&outgoing);
		header->cmsg_level      = SOL_SOCKET;
		header->cmsg_type       = SCM_RIGHTS;
		header->cmsg_len        = CMSG_LEN(sizeof(int));
		std::memcpy(CMSG_DATA(header), &config_fd, sizeof(config_fd));
		return sendmsg(message::kHelperSocketFd, &outgoing, MSG_NOSIGNAL) == 2;
	}

	auto SendState(ModelState state, const ScopedFd &config, const ScopedFd &model) -> bool {
		return message::SendMessage(message::kHelperSocketFd,
		                            {
		                                .model_state = state,
		                                .config_fd   = config.Get(),
		                                .model_fd = state == ModelState::kReady ? model.Get() : -1,
		                            });
	}

	[[noreturn]] void Stall() {
		for (;;) {
			pause();
		}
	}

}  // namespace

// The fake auth helper: speaks the descriptor protocol on fd 3 in the way `variant` names.
auto RunFakeHelper(std::string_view variant) -> int {
	const ScopedFd config(open("/proc/self/exe", O_RDONLY | O_CLOEXEC));
	const ScopedFd model(open("/proc/self/exe", O_RDONLY | O_CLOEXEC));
	const auto     result = [](bool sent) -> int {
		return sent ? EXIT_SUCCESS : EXIT_FAILURE;
	};
	if (variant == "ready") {
		return result(SendState(ModelState::kReady, config, model));
	}
	if (variant == "ready-then-fail") {
		// A failing exit status after a good message, which must not change the result.
		constexpr int failing_exit = 3;
		return SendState(ModelState::kReady, config, model) ? failing_exit : EXIT_FAILURE;
	}
	if (variant == "ready-then-stall") {
		if (SendState(ModelState::kReady, config, model)) {
			Stall();
		}
		return EXIT_FAILURE;
	}
	if (variant == "missing") {
		return result(SendState(ModelState::kMissing, config, model));
	}
	if (variant == "invalid") {
		return result(SendState(ModelState::kInvalidStorage, config, model));
	}
	if (variant == "twice") {
		bool sent = true;
		for (int round = 0; round < 2; ++round) {
			sent &= SendState(ModelState::kMissing, config, model);
		}
		return result(sent);
	}
	if (variant == "wrong-marker") {
		return result(SendWrongMarker(config.Get()));
	}
	return variant == "silent" ? EXIT_SUCCESS : EXIT_FAILURE;
}

auto main(int argc, char **argv) -> int {
	const auto arguments =
	    std::span(argv, argc > 0 ? static_cast<std::size_t>(argc) : std::size_t{0});
	if (arguments.size() == 3) {
		const std::string_view mode(arguments.subspan(1).front());
		if (mode == "--fake-helper") {
			return RunFakeHelper(arguments.back());
		}
		if (mode == "--fd3-probe") {
			return RunRuntimeSessionFd3Probe(arguments.back());
		}
		if (mode == "--stalled-helper") {
			return RunStalledHelper(arguments.back());
		}
	}

	bool ok = true;
	ok &= TestHelperForNonRoot();
	ok &= TestInProcessForRoot();
	ok &= TestMissingModelStillLoadsConfig();
	ok &= TestOpenFailure();
	ok &= TestConfigFailure();
	ok &= TestDescriptorValidation();
	ok &= TestOneShot();
	ok &= TestMissingDependencies();
	ok &= RunRuntimeSessionSpawnTests();
	ok &= RunRuntimeSessionDeadlineTests();
	return ok ? 0 : 1;
}
