#include "protocol/auth_helper_protocol.hpp"
#include "runtime/auth_helper_process.hpp"
#include "support/fd_io.hpp"
#include "test_support.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include <sys/wait.h>

auto RunAuthHelperFdTests() -> bool;
auto RunAuthHelperOutputTests() -> bool;

namespace {
	using howdy::test::expect;

	auto FakePartialReadError(void                                              *context,
	                          [[maybe_unused]] howdy::native::BoundedReadRequest request)
	    -> howdy::native::BoundedReadResult {
		(void)context;
		howdy::native::BoundedReadResult result;
		result.output       = "CONFIG_PATH=/tmp/partial\n";
		result.read_error   = true;
		result.error_number = EIO;
		return result;
	}

	auto ReadAuthHelperOutput(pid_t child_pid, int output_fd, std::string *output,
	                          howdy::pam::auth_helper_process::OutputReader reader = nullptr)
	    -> bool {
		auto operations         = howdy::pam::auth_helper_process::ProductionOperations();
		operations.read_bounded = reader;
		return howdy::pam::auth_helper_process::ReadOutput(
		    {.child_pid = child_pid, .output_fd = output_fd}, output, operations,
		    std::chrono::steady_clock::now() + std::chrono::seconds(10));
	}

	auto ReadFdToString(int fd) -> std::string {
		return howdy::native::ReadFdToStringBounded(
		           {.fd = fd, .max_bytes = howdy::pam::auth_helper_process::OutputLimit()})
		    .output;
	}

	using howdy::test::ScopedFd;

	struct TemporaryFile {
		std::string path;
		ScopedFd    fd;
	};

	auto OpenPipe(std::array<ScopedFd, 2> *fds) -> bool {
		std::array<int, 2> raw_fds{{-1, -1}};
		if (pipe(raw_fds.data()) != 0) {
			return false;
		}
		(*fds)[0].reset(raw_fds[0]);
		(*fds)[1].reset(raw_fds[1]);
		return true;
	}

	auto WriteAll(int fd, const std::string &data) -> bool {
		return howdy::native::WriteAllToFd(fd, data);
	}

	auto CreateTempFile(const std::string &label) -> std::optional<TemporaryFile> {
		const std::string template_path = "/tmp/howdy-auth-flow-" + label + "-XXXXXX";
		std::vector<char> path_buffer(template_path.begin(), template_path.end());
		path_buffer.push_back('\0');

		ScopedFd fd(mkstemp(path_buffer.data()));
		if (fd.get() < 0) {
			return std::nullopt;
		}
		return TemporaryFile{.path = path_buffer.data(), .fd = std::move(fd)};
	}

	auto ExpectFdReading() -> bool {
		bool                    ok = true;
		std::array<ScopedFd, 2> empty_pipe;
		ok &= expect(OpenPipe(&empty_pipe), "creates empty input pipe");
		empty_pipe[1].reset();
		ok &= expect(ReadFdToString(empty_pipe[0].get()).empty(), "reads empty input");
		ok &= expect(ReadFdToString(-1).empty(), "read failure returns collected empty output");

		std::array<ScopedFd, 2> small_pipe;
		ok &= expect(OpenPipe(&small_pipe), "creates small input pipe");
		const std::string small_output = "small helper output\n";
		ok &= expect(WriteAll(small_pipe[1].get(), small_output), "writes small helper output");
		small_pipe[1].reset();
		ok &= expect(ReadFdToString(small_pipe[0].get()) == small_output,
		             "reads complete small helper output");

		auto bounded_file = CreateTempFile("output");
		ok &= expect(bounded_file.has_value(), "creates bounded input file");
		if (!bounded_file.has_value()) {
			return false;
		}
		unlink(bounded_file->path.c_str());
		const std::string limit_output(16384, 'x');
		ok &= expect(WriteAll(bounded_file->fd.get(), limit_output),
		             "writes oversized helper output");
		ok &= expect(lseek(bounded_file->fd.get(), 0, SEEK_SET) == 0,
		             "rewinds oversized helper output");
		const std::string bounded_output = ReadFdToString(bounded_file->fd.get());
		ok &= expect(bounded_output == limit_output.substr(0, 9216),
		             "stops reading after bounded output threshold");

		return ok;
	}

	auto ExpectAuthHelperOutputLimitTerminatesChild() -> bool {
		bool                    ok = true;
		std::array<ScopedFd, 2> output_pipe;
		ok &= expect(OpenPipe(&output_pipe), "creates output-limit auth-helper pipe");
		if (!ok) {
			return false;
		}

		const std::string limit_output(howdy::pam::auth_helper_process::OutputLimit(), 'h');
		const pid_t       child_pid = fork();
		ok &= expect(child_pid >= 0, "forks output-limit auth-helper child");
		if (child_pid < 0) {
			return false;
		}
		if (child_pid == 0) {
			output_pipe[0].reset();
			const bool wrote = WriteAll(output_pipe[1].get(), limit_output);
			usleep(2000000);
			_exit(wrote ? 0 : 1);
		}

		output_pipe[1].reset();
		std::string helper_output;
		const auto  start = std::chrono::steady_clock::now();
		const bool  helper_ok =
		    ReadAuthHelperOutput(child_pid, output_pipe[0].get(), &helper_output);
		const auto elapsed = std::chrono::steady_clock::now() - start;

		ok &= expect(!helper_ok, "auth-helper output limit fails closed");
		ok &= expect(helper_output.empty(), "auth-helper output-limit data is not exposed");
		ok &= expect(elapsed < std::chrono::milliseconds(1500),
		             "auth-helper output limit returns before sleeping helper exits");

		int status              = 0;
		errno                   = 0;
		const pid_t wait_result = waitpid(child_pid, &status, WNOHANG);
		const int   wait_errno  = errno;
		ok &= expect(wait_result < 0 && wait_errno == ECHILD,
		             "output-limit auth-helper child is reaped");

		return ok;
	}

	auto ExpectAuthHelperOutputReadErrorTerminatesChild() -> bool {
		bool        ok        = true;
		const pid_t child_pid = fork();
		ok &= expect(child_pid >= 0, "forks read-error auth-helper child");
		if (child_pid < 0) {
			return false;
		}
		if (child_pid == 0) {
			usleep(2000000);
			_exit(0);
		}

		std::string helper_output;
		const auto  start     = std::chrono::steady_clock::now();
		const bool  helper_ok = ReadAuthHelperOutput(child_pid, -1, &helper_output);
		const auto  elapsed   = std::chrono::steady_clock::now() - start;

		ok &= expect(!helper_ok, "auth-helper read error fails closed");
		ok &= expect(helper_output.empty(), "auth-helper read error collects empty output");
		ok &= expect(elapsed < std::chrono::milliseconds(1500),
		             "auth-helper read error returns before sleeping helper exits");

		int   status      = 0;
		pid_t wait_result = 0;
		do {
			errno       = 0;
			wait_result = waitpid(child_pid, &status, WNOHANG);
		} while (wait_result < 0 && errno == EINTR);
		const int wait_errno = errno;
		ok &= expect(wait_result < 0 && wait_errno == ECHILD,
		             "read-error auth-helper child is reaped");

		if (wait_result == 0) {
			kill(child_pid, SIGKILL);
			do {
				errno       = 0;
				wait_result = waitpid(child_pid, &status, 0);
			} while (wait_result < 0 && errno == EINTR);
			ok &= expect(wait_result == child_pid,
			             "read-error auth-helper child cleanup reaps child");
		} else if (wait_result < 0 && wait_errno != ECHILD) {
			kill(child_pid, SIGKILL);
			do {
				errno       = 0;
				wait_result = waitpid(child_pid, &status, 0);
			} while (wait_result < 0 && errno == EINTR);
		}

		return ok;
	}

	auto ExpectAuthHelperOutputPartialReadErrorDiscardsOutput() -> bool {
		bool        ok        = true;
		const pid_t child_pid = fork();
		ok &= expect(child_pid >= 0, "forks partial-read-error auth-helper child");
		if (child_pid < 0) {
			return false;
		}
		if (child_pid == 0) {
			usleep(2000000);
			_exit(0);
		}

		std::string helper_output = "previous output";
		const bool  helper_ok =
		    ReadAuthHelperOutput(child_pid, -1, &helper_output, FakePartialReadError);

		ok &= expect(!helper_ok, "auth-helper partial read error fails closed");
		ok &= expect(helper_output.empty(), "auth-helper partial read error discards output");

		int   status      = 0;
		pid_t wait_result = 0;
		do {
			errno       = 0;
			wait_result = waitpid(child_pid, &status, WNOHANG);
		} while (wait_result < 0 && errno == EINTR);
		const int wait_errno = errno;
		ok &= expect(wait_result < 0 && wait_errno == ECHILD,
		             "partial-read-error auth-helper child is reaped");
		if (wait_result == 0) {
			kill(child_pid, SIGKILL);
			do {
				errno       = 0;
				wait_result = waitpid(child_pid, &status, 0);
			} while (wait_result < 0 && errno == EINTR);
		}

		return ok;
	}

	auto ReadCleanAuthHelperOutput(const std::string &child_output, std::string *read_output,
	                               int child_exit_status = EXIT_SUCCESS) -> std::optional<bool> {
		std::array<ScopedFd, 2> output_pipe;
		if (!OpenPipe(&output_pipe)) {
			return std::nullopt;
		}

		const pid_t child_pid = fork();
		if (child_pid < 0) {
			return std::nullopt;
		}
		if (child_pid == 0) {
			output_pipe[0].reset();
			const bool wrote = WriteAll(output_pipe[1].get(), child_output);
			output_pipe[1].reset();
			_exit(wrote ? child_exit_status : EXIT_FAILURE);
		}

		output_pipe[1].reset();
		return ReadAuthHelperOutput(child_pid, output_pipe[0].get(), read_output);
	}

	auto ExpectAuthHelperOutputChildFailureDiscardsOutput() -> bool {
		const std::string child_output  = "CONFIG_PATH=/run/howdy/config.ini\n"
		                                  "USER_MODELS_DIR=/run/howdy/models\n";
		std::string       actual_output = "previous output";
		const auto        read_result =
		    ReadCleanAuthHelperOutput(child_output, &actual_output, EXIT_FAILURE);

		bool ok = true;
		ok &= expect(read_result.has_value(), "failed auth-helper child exits cleanly");
		if (!read_result.has_value()) {
			return false;
		}
		ok &= expect(!*read_result, "failed auth-helper child output is rejected");
		ok &= expect(actual_output.empty(), "failed auth-helper child output is discarded");
		return ok;
	}

	auto ExpectAuthHelperOutputProtocolValidation() -> bool {
		using namespace howdy::native::auth_helper_protocol;
		namespace fs = std::filesystem;

		struct ProtocolCase {
			std::string name;
			std::string output;
			bool        expected_ok;
			std::string config_path;
			std::string user_models_dir;
		};

		const auto runtime_dir = PreparedRuntimeGenerationDir(PreparedRuntimeRoot(), getuid(),
		                                                      RuntimeGenerationSlot::kSlot0);
		const auto config_path = PreparedConfigPath(runtime_dir);
		const auto models_dir  = PreparedUserModelsDir(runtime_dir);
		const auto sibling_dir = PreparedRuntimeGenerationDir(PreparedRuntimeRoot(), getuid(),
		                                                      RuntimeGenerationSlot::kSlot1);
		const auto invalid_dir =
		    PreparedRuntimeRoot() / (PreparedRuntimeDirectoryPrefix(getuid()) + "sibling");
		const auto foreign_dir = PreparedRuntimeGenerationDir(
		    PreparedRuntimeRoot(), static_cast<uid_t>(getuid() + 1), RuntimeGenerationSlot::kSlot0);
		const auto output_for = [](const fs::path &config, const fs::path &models) -> std::string {
			return "CONFIG_PATH=" + config.string() + "\nUSER_MODELS_DIR=" + models.string() + "\n";
		};

		const std::vector<ProtocolCase> cases = {
		    {.name            = "valid canonical prepared-runtime output is accepted",
		     .output          = output_for(config_path, models_dir),
		     .expected_ok     = true,
		     .config_path     = config_path.string(),
		     .user_models_dir = models_dir.string()},
		    {.name        = "config path outside prepared runtime root is rejected",
		     .output      = output_for("/etc/howdy/config.ini", models_dir),
		     .expected_ok = false},
		    {.name        = "user-model directory outside prepared runtime root is rejected",
		     .output      = output_for(config_path, "/var/lib/howdy/models"),
		     .expected_ok = false},
		    {.name        = "sibling runtime paths are rejected",
		     .output      = output_for(config_path, PreparedUserModelsDir(sibling_dir)),
		     .expected_ok = false},
		    {.name = "invalid prepared runtime suffix is rejected",
		     .output =
		         output_for(PreparedConfigPath(invalid_dir), PreparedUserModelsDir(invalid_dir)),
		     .expected_ok = false},
		    {.name        = "config and models paths swapped are rejected",
		     .output      = output_for(runtime_dir / kPreparedUserModelsDirectoryName,
		                               runtime_dir / kPreparedConfigFileName),
		     .expected_ok = false},
		    {.name   = "unexpected nested path is rejected",
		     .output = output_for(runtime_dir / "nested" / kPreparedConfigFileName, models_dir),
		     .expected_ok = false},
		    {.name = "parent traversal path is rejected",
		     .output =
		         output_for(runtime_dir / ".." / "escaped" / kPreparedConfigFileName, models_dir),
		     .expected_ok = false},
		    {.name        = "equivalent noncanonical models path is rejected",
		     .output      = output_for(config_path, runtime_dir / "models" / ".." / "models"),
		     .expected_ok = false},
		    {.name = "foreign UID runtime path is rejected",
		     .output =
		         output_for(PreparedConfigPath(foreign_dir), PreparedUserModelsDir(foreign_dir)),
		     .expected_ok = false},
		    {.name        = "relative runtime paths are rejected",
		     .output      = output_for("relative/config.ini", "relative/models"),
		     .expected_ok = false},
		    {.name        = "duplicate CONFIG_PATH is rejected",
		     .output      = "CONFIG_PATH=" + config_path.string() +
		                    "\nCONFIG_PATH=" + (sibling_dir / kPreparedConfigFileName).string() +
		                    "\nUSER_MODELS_DIR=" + models_dir.string() + "\n",
		     .expected_ok = false},
		    {.name   = "duplicate USER_MODELS_DIR is rejected",
		     .output = "CONFIG_PATH=" + config_path.string() +
		               "\nUSER_MODELS_DIR=" + models_dir.string() +
		               "\nUSER_MODELS_DIR=" + PreparedUserModelsDir(sibling_dir).string() + "\n",
		     .expected_ok = false},
		    {.name        = "missing CONFIG_PATH is rejected",
		     .output      = "USER_MODELS_DIR=" + models_dir.string() + "\n",
		     .expected_ok = false},
		    {.name        = "missing USER_MODELS_DIR is rejected",
		     .output      = "CONFIG_PATH=" + config_path.string() + "\n",
		     .expected_ok = false},
		    {.name        = "empty CONFIG_PATH is rejected",
		     .output      = "CONFIG_PATH=\nUSER_MODELS_DIR=" + models_dir.string() + "\n",
		     .expected_ok = false},
		    {.name        = "empty USER_MODELS_DIR is rejected",
		     .output      = "CONFIG_PATH=" + config_path.string() + "\nUSER_MODELS_DIR=\n",
		     .expected_ok = false},
		    {.name        = "NOTICE line with valid required keys is rejected",
		     .output      = "NOTICE=ignored\n" + output_for(config_path, models_dir),
		     .expected_ok = false},
		    {.name        = "unknown key with valid required keys is rejected",
		     .output      = "UNKNOWN=ignored\n" + output_for(config_path, models_dir),
		     .expected_ok = false},
		    {.name        = "line without separator with valid required keys is rejected",
		     .output      = "CONFIG_PATH=" + config_path.string() +
		                    "\nhelper wrote stderr noise\n"
		                    "USER_MODELS_DIR=" +
		                    models_dir.string() + "\n",
		     .expected_ok = false},
		    {.name        = "required values containing equals are rejected by path contract",
		     .output      = output_for(runtime_dir / "config=debug.ini", models_dir),
		     .expected_ok = false},
		};

		bool ok = true;
		for (const auto &test_case : cases) {
			std::string actual_output;
			const auto  read_result = ReadCleanAuthHelperOutput(test_case.output, &actual_output);
			ok &= expect(read_result.has_value(), test_case.name + " helper exits cleanly");
			if (!read_result.has_value()) {
				continue;
			}

			ok &= expect(*read_result == test_case.expected_ok, test_case.name);
			if (!test_case.expected_ok) {
				ok &= expect(actual_output.empty(), test_case.name + " discards malformed output");
			}
			const auto parsed = howdy::pam::auth_helper_process::ParseOutput(test_case.output);
			ok &=
			    expect(parsed.valid == test_case.expected_ok, test_case.name + " parser validity");
			if (test_case.expected_ok) {
				ok &= expect(actual_output == test_case.output,
				             test_case.name + " exposes unchanged helper output");
				ok &= expect(parsed.config_path == test_case.config_path,
				             test_case.name + " config path parsed");
				ok &= expect(parsed.user_models_dir == test_case.user_models_dir,
				             test_case.name + " user models directory parsed");
			}
		}

		return ok;
	}

}  // namespace

auto RunAuthHelperFdTests() -> bool {
	return ExpectFdReading();
}

auto RunAuthHelperOutputTests() -> bool {
	bool ok = true;
	ok &= ExpectAuthHelperOutputLimitTerminatesChild();
	ok &= ExpectAuthHelperOutputReadErrorTerminatesChild();
	ok &= ExpectAuthHelperOutputPartialReadErrorDiscardsOutput();
	ok &= ExpectAuthHelperOutputChildFailureDiscardsOutput();
	ok &= ExpectAuthHelperOutputProtocolValidation();
	return ok;
}
