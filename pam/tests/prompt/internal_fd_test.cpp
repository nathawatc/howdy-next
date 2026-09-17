#include "prompt/internal_fd.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

#include <sys/wait.h>

namespace {
	using howdy::pam::detail::InternalFdOperations;
	using howdy::pam::detail::InternalPipe;
	using howdy::pam::detail::ScopedFd;
	using howdy::test::Expect;

	auto DescriptorIsClosed(int fd) -> bool {
		if (fd < 0) {
			return true;
		}
		errno = 0;
		if (fcntl(fd, F_GETFD) != -1) {
			return false;
		}
		return errno == EBADF;
	}

	auto DescriptorIsOpen(int fd) -> bool {
		if (fd < 0) {
			return false;
		}
		errno = 0;
		return fcntl(fd, F_GETFD) != -1;
	}

	auto OpenHighFd() -> int {
		const int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
		if (fd < 0 || fd > STDERR_FILENO) {
			return fd;
		}
		const int high_fd = fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
		(void)close(fd);
		return high_fd;
	}

	auto CloseStandardDescriptors() -> bool {
		return std::ranges::all_of(std::array{STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO},
		                           [](const int fd) -> bool {
			                           return close(fd) == 0 || errno == EBADF;
		                           });
	}

	template <typename Function>
	auto RunIsolated(Function function, std::string_view name) -> bool {
		const pid_t child_pid = fork();
		if (child_pid < 0) {
			return Expect(false, std::string(name) + " forks isolated child");
		}
		if (child_pid == 0) {
			std::exit(function() ? EXIT_SUCCESS : EXIT_FAILURE);
		}

		int status = 0;
		while (waitpid(child_pid, &status, 0) < 0) {
			if (errno == EINTR) {
				continue;
			}
			(void)kill(child_pid, SIGKILL);
			(void)waitpid(child_pid, nullptr, 0);
			return Expect(false, std::string(name) + " waits for isolated child");
		}
		return Expect(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS,
		              std::string(name) + " preserves parent descriptors");
	}

	auto TestScopedFdLifecycle() -> bool {
		bool     ok = true;
		ScopedFd default_fd;
		ok &= Expect(!default_fd.Valid() && default_fd.Get() == -1,
		             "default ScopedFd owns invalid descriptor");

		const int owned_fd = OpenHighFd();
		if (!Expect(owned_fd > STDERR_FILENO, "opens valid descriptor for ScopedFd")) {
			return false;
		}
		{
			ScopedFd fd(owned_fd);
			ok &= Expect(fd.Valid() && fd.Get() == owned_fd, "ScopedFd owns valid descriptor");
			ok &= Expect(DescriptorIsOpen(owned_fd),
			             "owned descriptor remains open before destruction");
		}
		ok &= Expect(DescriptorIsClosed(owned_fd), "ScopedFd closes owned descriptor");

		const int move_source_fd = OpenHighFd();
		if (!Expect(move_source_fd > STDERR_FILENO, "opens move-constructor descriptor")) {
			return false;
		}
		{
			ScopedFd source(move_source_fd);
			ScopedFd moved(std::move(source));
			ok &= Expect(moved.Valid() && moved.Get() == move_source_fd,
			             "move constructor transfers descriptor");
		}
		ok &= Expect(DescriptorIsClosed(move_source_fd),
		             "moved ScopedFd closes transferred descriptor");

		const int previous_fd = OpenHighFd();
		const int incoming_fd = OpenHighFd();
		if (!Expect(previous_fd > STDERR_FILENO && incoming_fd > STDERR_FILENO,
		            "opens move-assignment descriptors")) {
			if (previous_fd >= 0) {
				(void)close(previous_fd);
			}
			if (incoming_fd >= 0 && incoming_fd != previous_fd) {
				(void)close(incoming_fd);
			}
			return false;
		}
		{
			ScopedFd destination(previous_fd);
			ScopedFd source(incoming_fd);
			destination = std::move(source);
			ok &= Expect(destination.Valid() && destination.Get() == incoming_fd,
			             "move assignment transfers incoming descriptor");
			ok &= Expect(DescriptorIsClosed(previous_fd),
			             "move assignment closes previous owned descriptor");
			ok &= Expect(DescriptorIsOpen(incoming_fd),
			             "move assignment keeps incoming descriptor open");
		}
		ok &= Expect(DescriptorIsClosed(incoming_fd),
		             "move-assigned ScopedFd closes incoming descriptor");

		const int released_fd = OpenHighFd();
		if (!Expect(released_fd > STDERR_FILENO, "opens release descriptor")) {
			return false;
		}
		{
			ScopedFd fd(released_fd);
			ok &= Expect(fd.Release() == released_fd && !fd.Valid(),
			             "release returns descriptor and invalidates ScopedFd");
			ok &= Expect(DescriptorIsOpen(released_fd),
			             "released descriptor stays open after release");
		}
		ok &= Expect(DescriptorIsOpen(released_fd),
		             "released descriptor is not closed by destruction");
		(void)close(released_fd);
		return ok &&
		       Expect(DescriptorIsClosed(released_fd), "released descriptor closes explicitly");
	}

	auto TestScopedFdResetAndClose() -> bool {
		bool      ok             = true;
		const int initial_fd     = OpenHighFd();
		const int replacement_fd = OpenHighFd();
		if (!Expect(initial_fd > STDERR_FILENO && replacement_fd > STDERR_FILENO,
		            "opens descriptors for Reset and Close test")) {
			if (initial_fd >= 0) {
				(void)close(initial_fd);
			}
			if (replacement_fd >= 0) {
				(void)close(replacement_fd);
			}
			return false;
		}

		ScopedFd fd(initial_fd);
		fd.Reset(replacement_fd);
		ok &= Expect(DescriptorIsClosed(initial_fd), "Reset closes previous descriptor");
		ok &= Expect(fd.Valid() && fd.Get() == replacement_fd, "Reset sets new descriptor");
		ok &= Expect(fd.Close(), "Close closes descriptor successfully");
		ok &= Expect(DescriptorIsClosed(replacement_fd), "Close closes descriptor");
		ok &= Expect(!fd.Valid() && fd.Get() == -1, "Close invalidates ScopedFd");

		const int reset_fd = OpenHighFd();
		if (!Expect(reset_fd > STDERR_FILENO, "opens descriptor for Reset default argument test")) {
			return false;
		}
		fd.Reset(reset_fd);
		ok &= Expect(fd.Valid() && fd.Get() == reset_fd, "Reset sets descriptor");
		fd.Reset();
		ok &= Expect(!fd.Valid() && fd.Get() == -1,
		             "Reset with default argument invalidates ScopedFd");
		ok &= Expect(DescriptorIsClosed(reset_fd), "Reset with default argument closes descriptor");
		return ok;
	}

	auto TestInternalPipeValidity() -> bool {
		bool      ok       = true;
		const int read_fd  = OpenHighFd();
		const int write_fd = OpenHighFd();
		if (!Expect(read_fd > STDERR_FILENO && write_fd > STDERR_FILENO && read_fd != write_fd,
		            "opens distinct high descriptors for valid InternalPipe")) {
			if (read_fd >= 0) {
				(void)close(read_fd);
			}
			if (write_fd >= 0 && write_fd != read_fd) {
				(void)close(write_fd);
			}
			return false;
		}
		{
			InternalPipe pipe{.read = ScopedFd(read_fd), .write = ScopedFd(write_fd)};
			ok &= Expect(pipe.Valid(), "distinct high InternalPipe endpoints are valid");
		}

		const int invalid_write_fd = OpenHighFd();
		if (!Expect(invalid_write_fd > STDERR_FILENO, "opens endpoint for invalid InternalPipe")) {
			return false;
		}
		{
			InternalPipe pipe{.read = ScopedFd(), .write = ScopedFd(invalid_write_fd)};
			ok &= Expect(!pipe.Valid(), "invalid InternalPipe endpoint fails validation");
		}
		return ok && Expect(DescriptorIsClosed(invalid_write_fd),
		                    "invalid InternalPipe endpoint is still owned and closed");
	}

	auto TestInternalPipeLowAndIdenticalEndpoints() -> bool {
		if (!CloseStandardDescriptors()) {
			return false;
		}
		for (const int low_fd : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) {
			const int high_fd = OpenHighFd();
			if (high_fd <= STDERR_FILENO) {
				return false;
			}
			{
				InternalPipe pipe{.read = ScopedFd(low_fd), .write = ScopedFd(high_fd)};
				if (pipe.Valid()) {
					return false;
				}
			}
			if (!DescriptorIsClosed(high_fd)) {
				return false;
			}
		}

		const int identical_fd = OpenHighFd();
		if (identical_fd <= STDERR_FILENO) {
			return false;
		}
		{
			InternalPipe pipe{.read = ScopedFd(identical_fd), .write = ScopedFd(identical_fd)};
			if (pipe.Valid()) {
				return false;
			}
		}
		return DescriptorIsClosed(identical_fd);
	}

	struct DuplicateContext {
		std::vector<std::pair<int, int>> requests;
		std::size_t                      calls      = 0;
		bool                             fail       = false;
		bool                             return_low = false;
		int                              result_low = STDERR_FILENO;
	};

	auto DuplicateForTest(void *context, int fd, int minimum_fd) -> int {
		auto &state = *static_cast<DuplicateContext *>(context);
		++state.calls;
		state.requests.emplace_back(fd, minimum_fd);
		if (state.fail) {
			errno = EMFILE;
			return -1;
		}
		if (state.return_low) {
			return state.result_low;
		}
		return fcntl(fd, F_DUPFD_CLOEXEC, minimum_fd);
	}

	auto TestNormalizeHighAndInvalid() -> bool {
		bool                       ok = true;
		DuplicateContext           context;
		const InternalFdOperations operations{.context = &context, .duplicate = DuplicateForTest};

		ScopedFd invalid;
		auto     still_invalid =
		    howdy::pam::detail::NormalizeInternalFd(std::move(invalid), &operations);
		ok &= Expect(!still_invalid.Valid(), "invalid normalize input remains invalid");
		ok &= Expect(context.calls == 0, "invalid normalize input skips duplicate callback");

		const int high_fd = OpenHighFd();
		if (!Expect(high_fd > STDERR_FILENO, "opens high normalize input")) {
			return false;
		}
		{
			ScopedFd input(high_fd);
			auto     normalized =
			    howdy::pam::detail::NormalizeInternalFd(std::move(input), &operations);
			ok &= Expect(normalized.Valid() && normalized.Get() == high_fd,
			             "high normalize input passes through unchanged");
			ok &= Expect(context.calls == 0, "high normalize input skips duplicate callback");
		}
		return ok &&
		       Expect(DescriptorIsClosed(high_fd), "high normalize descriptor closes by ownership");
	}

	auto TestNormalizeLowAndFailures() -> bool {
		if (!CloseStandardDescriptors()) {
			return false;
		}

		const int production_low_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
		if (production_low_fd != STDIN_FILENO) {
			return false;
		}
		{
			auto normalized = howdy::pam::detail::NormalizeInternalFd(ScopedFd(production_low_fd));
			if (!normalized.Valid() || normalized.Get() <= STDERR_FILENO) {
				return false;
			}
		}
		if (!DescriptorIsClosed(production_low_fd)) {
			return false;
		}

		std::array<int, 3> low_fds{};
		for (int expected_fd = STDIN_FILENO; expected_fd <= STDERR_FILENO; ++expected_fd) {
			low_fds[static_cast<std::size_t>(expected_fd)] =
			    open("/dev/null", O_RDONLY | O_CLOEXEC);
			if (low_fds[static_cast<std::size_t>(expected_fd)] != expected_fd) {
				return false;
			}
		}
		DuplicateContext           context;
		const InternalFdOperations operations{.context = &context, .duplicate = DuplicateForTest};
		for (const int low_fd : low_fds) {
			ScopedFd input(low_fd);
			auto     normalized =
			    howdy::pam::detail::NormalizeInternalFd(std::move(input), &operations);
			if (!normalized.Valid() || normalized.Get() <= STDERR_FILENO) {
				return false;
			}
		}
		if (context.requests !=
		    std::vector<std::pair<int, int>>{{STDIN_FILENO, STDERR_FILENO + 1},
		                                     {STDOUT_FILENO, STDERR_FILENO + 1},
		                                     {STDERR_FILENO, STDERR_FILENO + 1}}) {
			return false;
		}
		for (const int low_fd : low_fds) {
			if (!DescriptorIsClosed(low_fd)) {
				return false;
			}
		}

		const int missing_callback_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
		if (missing_callback_fd != STDIN_FILENO) {
			return false;
		}
		const InternalFdOperations missing_callback_operations{.context = &context};
		auto missing_callback_result = howdy::pam::detail::NormalizeInternalFd(
		    ScopedFd(missing_callback_fd), &missing_callback_operations);
		if (missing_callback_result.Valid() || !DescriptorIsClosed(missing_callback_fd)) {
			return false;
		}

		const int failed_duplicate_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
		if (failed_duplicate_fd != STDIN_FILENO) {
			return false;
		}
		DuplicateContext failed_duplicate;
		failed_duplicate.fail = true;
		const InternalFdOperations failed_duplicate_operations{.context   = &failed_duplicate,
		                                                       .duplicate = DuplicateForTest};
		auto failed_duplicate_result = howdy::pam::detail::NormalizeInternalFd(
		    ScopedFd(failed_duplicate_fd), &failed_duplicate_operations);
		if (failed_duplicate_result.Valid() || failed_duplicate.calls != 1 ||
		    !DescriptorIsClosed(failed_duplicate_fd)) {
			return false;
		}

		const int low_result_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
		if (low_result_fd != STDIN_FILENO) {
			return false;
		}
		DuplicateContext low_result;
		low_result.return_low = true;
		const InternalFdOperations low_result_operations{.context   = &low_result,
		                                                 .duplicate = DuplicateForTest};
		auto                       low_result_descriptor = howdy::pam::detail::NormalizeInternalFd(
		    ScopedFd(low_result_fd), &low_result_operations);
		return !low_result_descriptor.Valid() && low_result.calls == 1 &&
		       DescriptorIsClosed(low_result_fd);
	}

	struct PipeContext {
		std::array<int, 2>               raw_fds{{-1, -1}};
		std::array<int, 2>               duplicate_fds{{-1, -1}};
		std::vector<std::pair<int, int>> duplicate_requests;
		int                              create_calls        = 0;
		std::size_t                      duplicate_calls     = 0;
		std::size_t                      fail_duplicate_call = 0;
	};

	auto RecordPipeRawFds(PipeContext &state, int *pipe_fds) -> void {
		state.raw_fds = {pipe_fds[0], pipe_fds[1]};
	}

	auto CreateHighPipe(void *context, int *pipe_fds, int flags) -> int {
		auto &state = *static_cast<PipeContext *>(context);
		++state.create_calls;
		if (pipe2(pipe_fds, flags) != 0) {
			return -1;
		}
		for (int &pipe_fd : std::span(pipe_fds, 2)) {
			if (pipe_fd <= STDERR_FILENO) {
				const int high_fd = fcntl(pipe_fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
				if (high_fd < 0) {
					(void)close(pipe_fds[0]);
					(void)close(pipe_fds[1]);
					pipe_fds[0] = -1;
					pipe_fds[1] = -1;
					return -1;
				}
				(void)close(pipe_fd);
				pipe_fd = high_fd;
			}
		}
		RecordPipeRawFds(state, pipe_fds);
		return 0;
	}

	auto CreateRawPipe(void *context, int *pipe_fds, int flags) -> int {
		auto &state = *static_cast<PipeContext *>(context);
		++state.create_calls;
		const int result = pipe2(pipe_fds, flags);
		if (result == 0) {
			RecordPipeRawFds(state, pipe_fds);
		}
		return result;
	}

	auto CreateFailedPipe(void *context, int * /*pipe_fds*/, int flags) -> int {
		(void)flags;
		auto &state = *static_cast<PipeContext *>(context);
		++state.create_calls;
		errno = EMFILE;
		return -1;
	}

	auto CreateInvalidPipe(void *context, int *pipe_fds, int flags) -> int {
		(void)flags;
		auto &state = *static_cast<PipeContext *>(context);
		++state.create_calls;
		pipe_fds[0] = OpenHighFd();
		pipe_fds[1] = -1;
		RecordPipeRawFds(state, pipe_fds);
		return pipe_fds[0] < 0 ? -1 : 0;
	}

	auto CreateIdenticalPipe(void *context, int *pipe_fds, int flags) -> int {
		(void)flags;
		auto &state = *static_cast<PipeContext *>(context);
		++state.create_calls;
		const int fd = OpenHighFd();
		pipe_fds[0]  = fd;
		pipe_fds[1]  = fd;
		RecordPipeRawFds(state, pipe_fds);
		return fd < 0 ? -1 : 0;
	}

	auto DuplicatePipeFd(void *context, int fd, int minimum_fd) -> int {
		auto &state = *static_cast<PipeContext *>(context);
		++state.duplicate_calls;
		state.duplicate_requests.emplace_back(fd, minimum_fd);
		if (state.fail_duplicate_call == state.duplicate_calls) {
			errno = EMFILE;
			return -1;
		}
		const int duplicate_fd = fcntl(fd, F_DUPFD_CLOEXEC, minimum_fd);
		if (state.duplicate_calls <= state.duplicate_fds.size()) {
			state.duplicate_fds[state.duplicate_calls - 1] = duplicate_fd;
		}
		return duplicate_fd;
	}

	auto RecordedPipeDescriptorsAreClosed(const PipeContext &state) -> bool {
		return std::ranges::all_of(std::array{state.raw_fds[0], state.raw_fds[1],
		                                      state.duplicate_fds[0], state.duplicate_fds[1]},
		                           [](const int fd) -> bool {
			                           return DescriptorIsClosed(fd);
		                           });
	}

	auto TestProductionInternalPipe() -> bool {
		auto pipe = howdy::pam::detail::CreateInternalPipe(O_CLOEXEC);
		return pipe.Valid() && pipe.read.Get() > STDERR_FILENO &&
		       pipe.write.Get() > STDERR_FILENO && pipe.read.Get() != pipe.write.Get();
	}

	auto TestCreateInternalPipePaths() -> bool {
		bool ok = true;

		PipeContext                missing_context;
		const InternalFdOperations missing_create_operations{.context   = &missing_context,
		                                                     .duplicate = DuplicatePipeFd};
		auto                       missing_create =
		    howdy::pam::detail::CreateInternalPipe(O_CLOEXEC, &missing_create_operations);
		ok &= Expect(!missing_create.Valid() && missing_context.create_calls == 0,
		             "missing create_pipe callback fails closed");

		PipeContext                failed_context;
		const InternalFdOperations failed_create_operations{.context     = &failed_context,
		                                                    .duplicate   = DuplicatePipeFd,
		                                                    .create_pipe = CreateFailedPipe};
		auto                       failed_create =
		    howdy::pam::detail::CreateInternalPipe(O_CLOEXEC, &failed_create_operations);
		ok &= Expect(!failed_create.Valid() && failed_context.create_calls == 1,
		             "create_pipe failure fails closed");
		ok &= Expect(RecordedPipeDescriptorsAreClosed(failed_context),
		             "create_pipe failure leaves no descriptors to close");

		PipeContext                high_context;
		const InternalFdOperations high_operations{
		    .context = &high_context, .duplicate = DuplicatePipeFd, .create_pipe = CreateHighPipe};
		{
			auto high_pipe = howdy::pam::detail::CreateInternalPipe(O_CLOEXEC, &high_operations);
			ok &= Expect(high_pipe.Valid() && high_pipe.read.Get() > STDERR_FILENO &&
			                 high_pipe.write.Get() > STDERR_FILENO &&
			                 high_pipe.read.Get() != high_pipe.write.Get(),
			             "high-descriptor internal pipe succeeds");
			ok &= Expect(high_context.duplicate_calls == 0,
			             "high-descriptor internal pipe skips normalization callback");
		}
		ok &= Expect(RecordedPipeDescriptorsAreClosed(high_context),
		             "successful high-descriptor pipe closes owned endpoints");

		PipeContext                invalid_context;
		const InternalFdOperations invalid_operations{.context     = &invalid_context,
		                                              .duplicate   = DuplicatePipeFd,
		                                              .create_pipe = CreateInvalidPipe};
		{
			auto invalid_pipe =
			    howdy::pam::detail::CreateInternalPipe(O_CLOEXEC, &invalid_operations);
			ok &= Expect(!invalid_pipe.Valid(), "invalid final pipe endpoint fails closed");
		}
		ok &= Expect(RecordedPipeDescriptorsAreClosed(invalid_context),
		             "invalid final pipe endpoint does not leak");

		PipeContext                identical_context;
		const InternalFdOperations identical_operations{.context     = &identical_context,
		                                                .duplicate   = DuplicatePipeFd,
		                                                .create_pipe = CreateIdenticalPipe};
		{
			auto identical_pipe =
			    howdy::pam::detail::CreateInternalPipe(O_CLOEXEC, &identical_operations);
			ok &= Expect(!identical_pipe.Valid(), "identical final pipe endpoints fail closed");
		}
		return ok && Expect(RecordedPipeDescriptorsAreClosed(identical_context),
		                    "identical final pipe endpoints do not leak");
	}

	auto TestCreateInternalPipeLowAndNormalizationFailures() -> bool {
		if (!CloseStandardDescriptors()) {
			return false;
		}

		PipeContext                success_context;
		const InternalFdOperations success_operations{.context     = &success_context,
		                                              .duplicate   = DuplicatePipeFd,
		                                              .create_pipe = CreateRawPipe};
		{
			auto pipe = howdy::pam::detail::CreateInternalPipe(O_CLOEXEC, &success_operations);
			if (!pipe.Valid() || pipe.read.Get() <= STDERR_FILENO ||
			    pipe.write.Get() <= STDERR_FILENO || pipe.read.Get() == pipe.write.Get()) {
				return false;
			}
		}
		if (success_context.duplicate_calls != 2 ||
		    success_context.duplicate_requests !=
		        std::vector<std::pair<int, int>>{{STDIN_FILENO, STDERR_FILENO + 1},
		                                         {STDOUT_FILENO, STDERR_FILENO + 1}} ||
		    !RecordedPipeDescriptorsAreClosed(success_context)) {
			return false;
		}

		for (const std::size_t failure_call : {std::size_t{1}, std::size_t{2}}) {
			PipeContext failure_context;
			failure_context.fail_duplicate_call = failure_call;
			const InternalFdOperations failure_operations{.context     = &failure_context,
			                                              .duplicate   = DuplicatePipeFd,
			                                              .create_pipe = CreateRawPipe};
			{
				auto pipe = howdy::pam::detail::CreateInternalPipe(O_CLOEXEC, &failure_operations);
				if (pipe.Valid() || failure_context.duplicate_calls != failure_call) {
					return false;
				}
			}
			if (!RecordedPipeDescriptorsAreClosed(failure_context)) {
				return false;
			}
		}
		return true;
	}
}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= TestScopedFdLifecycle();
	ok &= TestScopedFdResetAndClose();
	ok &= TestInternalPipeValidity();
	ok &= RunIsolated(TestInternalPipeLowAndIdenticalEndpoints,
	                  "low and identical InternalPipe endpoints");
	ok &= TestNormalizeHighAndInvalid();
	ok &= RunIsolated(TestNormalizeLowAndFailures, "low and failing descriptor normalization");
	ok &= RunIsolated(TestProductionInternalPipe, "production internal pipe operations");
	ok &= TestCreateInternalPipePaths();
	ok &= RunIsolated(TestCreateInternalPipeLowAndNormalizationFailures,
	                  "low and failing internal pipe creation");
	return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
