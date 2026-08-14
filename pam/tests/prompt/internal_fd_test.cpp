#include "internal_fd.hpp"
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
	using howdy::test::expect;

	auto descriptor_is_closed(int fd) -> bool {
		if (fd < 0) {
			return true;
		}
		errno = 0;
		if (fcntl(fd, F_GETFD) != -1) {
			return false;
		}
		return errno == EBADF;
	}

	auto descriptor_is_open(int fd) -> bool {
		if (fd < 0) {
			return false;
		}
		errno = 0;
		return fcntl(fd, F_GETFD) != -1;
	}

	auto open_high_fd() -> int {
		const int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
		if (fd < 0 || fd > STDERR_FILENO) {
			return fd;
		}
		const int high_fd = fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
		(void)close(fd);
		return high_fd;
	}

	auto close_standard_descriptors() -> bool {
		return std::ranges::all_of(std::array{STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO},
		                           [](const int fd) -> bool {
			                           return close(fd) == 0 || errno == EBADF;
		                           });
	}

	template <typename Function>
	auto run_isolated(Function function, std::string_view name) -> bool {
		const pid_t child_pid = fork();
		if (child_pid < 0) {
			return expect(false, std::string(name) + " forks isolated child");
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
			return expect(false, std::string(name) + " waits for isolated child");
		}
		return expect(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS,
		              std::string(name) + " preserves parent descriptors");
	}

	auto test_scoped_fd_lifecycle() -> bool {
		bool     ok = true;
		ScopedFd default_fd;
		ok &= expect(!default_fd.valid() && default_fd.get() == -1,
		             "default ScopedFd owns invalid descriptor");

		const int owned_fd = open_high_fd();
		if (!expect(owned_fd > STDERR_FILENO, "opens valid descriptor for ScopedFd")) {
			return false;
		}
		{
			ScopedFd fd(owned_fd);
			ok &= expect(fd.valid() && fd.get() == owned_fd, "ScopedFd owns valid descriptor");
			ok &= expect(descriptor_is_open(owned_fd),
			             "owned descriptor remains open before destruction");
		}
		ok &= expect(descriptor_is_closed(owned_fd), "ScopedFd closes owned descriptor");

		const int move_source_fd = open_high_fd();
		if (!expect(move_source_fd > STDERR_FILENO, "opens move-constructor descriptor")) {
			return false;
		}
		{
			ScopedFd source(move_source_fd);
			ScopedFd moved(std::move(source));
			ok &= expect(moved.valid() && moved.get() == move_source_fd,
			             "move constructor transfers descriptor");
		}
		ok &= expect(descriptor_is_closed(move_source_fd),
		             "moved ScopedFd closes transferred descriptor");

		const int previous_fd = open_high_fd();
		const int incoming_fd = open_high_fd();
		if (!expect(previous_fd > STDERR_FILENO && incoming_fd > STDERR_FILENO,
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
			ok &= expect(destination.valid() && destination.get() == incoming_fd,
			             "move assignment transfers incoming descriptor");
			ok &= expect(descriptor_is_closed(previous_fd),
			             "move assignment closes previous owned descriptor");
			ok &= expect(descriptor_is_open(incoming_fd),
			             "move assignment keeps incoming descriptor open");
		}
		ok &= expect(descriptor_is_closed(incoming_fd),
		             "move-assigned ScopedFd closes incoming descriptor");

		const int released_fd = open_high_fd();
		if (!expect(released_fd > STDERR_FILENO, "opens release descriptor")) {
			return false;
		}
		{
			ScopedFd fd(released_fd);
			ok &= expect(fd.release() == released_fd && !fd.valid(),
			             "release returns descriptor and invalidates ScopedFd");
			ok &= expect(descriptor_is_open(released_fd),
			             "released descriptor stays open after release");
		}
		ok &= expect(descriptor_is_open(released_fd),
		             "released descriptor is not closed by destruction");
		(void)close(released_fd);
		return ok &&
		       expect(descriptor_is_closed(released_fd), "released descriptor closes explicitly");
	}

	auto test_internal_pipe_validity() -> bool {
		bool      ok       = true;
		const int read_fd  = open_high_fd();
		const int write_fd = open_high_fd();
		if (!expect(read_fd > STDERR_FILENO && write_fd > STDERR_FILENO && read_fd != write_fd,
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
			ok &= expect(pipe.valid(), "distinct high InternalPipe endpoints are valid");
		}

		const int invalid_write_fd = open_high_fd();
		if (!expect(invalid_write_fd > STDERR_FILENO, "opens endpoint for invalid InternalPipe")) {
			return false;
		}
		{
			InternalPipe pipe{.read = ScopedFd(), .write = ScopedFd(invalid_write_fd)};
			ok &= expect(!pipe.valid(), "invalid InternalPipe endpoint fails validation");
		}
		return ok && expect(descriptor_is_closed(invalid_write_fd),
		                    "invalid InternalPipe endpoint is still owned and closed");
	}

	auto test_internal_pipe_low_and_identical_endpoints() -> bool {
		if (!close_standard_descriptors()) {
			return false;
		}
		for (const int low_fd : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) {
			const int high_fd = open_high_fd();
			if (high_fd <= STDERR_FILENO) {
				return false;
			}
			{
				InternalPipe pipe{.read = ScopedFd(low_fd), .write = ScopedFd(high_fd)};
				if (pipe.valid()) {
					return false;
				}
			}
			if (!descriptor_is_closed(high_fd)) {
				return false;
			}
		}

		const int identical_fd = open_high_fd();
		if (identical_fd <= STDERR_FILENO) {
			return false;
		}
		{
			InternalPipe pipe{.read = ScopedFd(identical_fd), .write = ScopedFd(identical_fd)};
			if (pipe.valid()) {
				return false;
			}
		}
		return descriptor_is_closed(identical_fd);
	}

	struct DuplicateContext {
		std::vector<std::pair<int, int>> requests;
		std::size_t                      calls      = 0;
		bool                             fail       = false;
		bool                             return_low = false;
		int                              result_low = STDERR_FILENO;
	};

	auto duplicate_for_test(void *context, int fd, int minimum_fd) -> int {
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

	auto test_normalize_high_and_invalid() -> bool {
		bool                       ok = true;
		DuplicateContext           context;
		const InternalFdOperations operations{.context = &context, .duplicate = duplicate_for_test};

		ScopedFd invalid;
		auto     still_invalid =
		    howdy::pam::detail::normalize_internal_fd(std::move(invalid), &operations);
		ok &= expect(!still_invalid.valid(), "invalid normalize input remains invalid");
		ok &= expect(context.calls == 0, "invalid normalize input skips duplicate callback");

		const int high_fd = open_high_fd();
		if (!expect(high_fd > STDERR_FILENO, "opens high normalize input")) {
			return false;
		}
		{
			ScopedFd input(high_fd);
			auto     normalized =
			    howdy::pam::detail::normalize_internal_fd(std::move(input), &operations);
			ok &= expect(normalized.valid() && normalized.get() == high_fd,
			             "high normalize input passes through unchanged");
			ok &= expect(context.calls == 0, "high normalize input skips duplicate callback");
		}
		return ok && expect(descriptor_is_closed(high_fd),
		                    "high normalize descriptor closes by ownership");
	}

	auto test_normalize_low_and_failures() -> bool {
		if (!close_standard_descriptors()) {
			return false;
		}

		const int production_low_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
		if (production_low_fd != STDIN_FILENO) {
			return false;
		}
		{
			auto normalized =
			    howdy::pam::detail::normalize_internal_fd(ScopedFd(production_low_fd));
			if (!normalized.valid() || normalized.get() <= STDERR_FILENO) {
				return false;
			}
		}
		if (!descriptor_is_closed(production_low_fd)) {
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
		const InternalFdOperations operations{.context = &context, .duplicate = duplicate_for_test};
		for (const int low_fd : low_fds) {
			ScopedFd input(low_fd);
			auto     normalized =
			    howdy::pam::detail::normalize_internal_fd(std::move(input), &operations);
			if (!normalized.valid() || normalized.get() <= STDERR_FILENO) {
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
			if (!descriptor_is_closed(low_fd)) {
				return false;
			}
		}

		const int missing_callback_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
		if (missing_callback_fd != STDIN_FILENO) {
			return false;
		}
		const InternalFdOperations missing_callback_operations{.context = &context};
		auto missing_callback_result = howdy::pam::detail::normalize_internal_fd(
		    ScopedFd(missing_callback_fd), &missing_callback_operations);
		if (missing_callback_result.valid() || !descriptor_is_closed(missing_callback_fd)) {
			return false;
		}

		const int failed_duplicate_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
		if (failed_duplicate_fd != STDIN_FILENO) {
			return false;
		}
		DuplicateContext failed_duplicate;
		failed_duplicate.fail = true;
		const InternalFdOperations failed_duplicate_operations{.context   = &failed_duplicate,
		                                                       .duplicate = duplicate_for_test};
		auto failed_duplicate_result = howdy::pam::detail::normalize_internal_fd(
		    ScopedFd(failed_duplicate_fd), &failed_duplicate_operations);
		if (failed_duplicate_result.valid() || failed_duplicate.calls != 1 ||
		    !descriptor_is_closed(failed_duplicate_fd)) {
			return false;
		}

		const int low_result_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
		if (low_result_fd != STDIN_FILENO) {
			return false;
		}
		DuplicateContext low_result;
		low_result.return_low = true;
		const InternalFdOperations low_result_operations{.context   = &low_result,
		                                                 .duplicate = duplicate_for_test};
		auto low_result_descriptor = howdy::pam::detail::normalize_internal_fd(
		    ScopedFd(low_result_fd), &low_result_operations);
		return !low_result_descriptor.valid() && low_result.calls == 1 &&
		       descriptor_is_closed(low_result_fd);
	}

	struct PipeContext {
		std::array<int, 2>               raw_fds{{-1, -1}};
		std::array<int, 2>               duplicate_fds{{-1, -1}};
		std::vector<std::pair<int, int>> duplicate_requests;
		int                              create_calls        = 0;
		std::size_t                      duplicate_calls     = 0;
		std::size_t                      fail_duplicate_call = 0;
	};

	auto record_pipe_raw_fds(PipeContext &state, int *pipe_fds) -> void {
		state.raw_fds = {pipe_fds[0], pipe_fds[1]};
	}

	auto create_high_pipe(void *context, int *pipe_fds, int flags) -> int {
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
		record_pipe_raw_fds(state, pipe_fds);
		return 0;
	}

	auto create_raw_pipe(void *context, int *pipe_fds, int flags) -> int {
		auto &state = *static_cast<PipeContext *>(context);
		++state.create_calls;
		const int result = pipe2(pipe_fds, flags);
		if (result == 0) {
			record_pipe_raw_fds(state, pipe_fds);
		}
		return result;
	}

	auto create_failed_pipe(void *context, int * /*pipe_fds*/, int flags) -> int {
		(void)flags;
		auto &state = *static_cast<PipeContext *>(context);
		++state.create_calls;
		errno = EMFILE;
		return -1;
	}

	auto create_invalid_pipe(void *context, int *pipe_fds, int flags) -> int {
		(void)flags;
		auto &state = *static_cast<PipeContext *>(context);
		++state.create_calls;
		pipe_fds[0] = open_high_fd();
		pipe_fds[1] = -1;
		record_pipe_raw_fds(state, pipe_fds);
		return pipe_fds[0] < 0 ? -1 : 0;
	}

	auto create_identical_pipe(void *context, int *pipe_fds, int flags) -> int {
		(void)flags;
		auto &state = *static_cast<PipeContext *>(context);
		++state.create_calls;
		const int fd = open_high_fd();
		pipe_fds[0]  = fd;
		pipe_fds[1]  = fd;
		record_pipe_raw_fds(state, pipe_fds);
		return fd < 0 ? -1 : 0;
	}

	auto duplicate_pipe_fd(void *context, int fd, int minimum_fd) -> int {
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

	auto recorded_pipe_descriptors_are_closed(const PipeContext &state) -> bool {
		return std::ranges::all_of(std::array{state.raw_fds[0], state.raw_fds[1],
		                                      state.duplicate_fds[0], state.duplicate_fds[1]},
		                           [](const int fd) -> bool {
			                           return descriptor_is_closed(fd);
		                           });
	}

	auto test_production_internal_pipe() -> bool {
		auto pipe = howdy::pam::detail::create_internal_pipe(O_CLOEXEC);
		return pipe.valid() && pipe.read.get() > STDERR_FILENO &&
		       pipe.write.get() > STDERR_FILENO && pipe.read.get() != pipe.write.get();
	}

	auto test_create_internal_pipe_paths() -> bool {
		bool ok = true;

		PipeContext                missing_context;
		const InternalFdOperations missing_create_operations{.context   = &missing_context,
		                                                     .duplicate = duplicate_pipe_fd};
		auto                       missing_create =
		    howdy::pam::detail::create_internal_pipe(O_CLOEXEC, &missing_create_operations);
		ok &= expect(!missing_create.valid() && missing_context.create_calls == 0,
		             "missing create_pipe callback fails closed");

		PipeContext                failed_context;
		const InternalFdOperations failed_create_operations{.context     = &failed_context,
		                                                    .duplicate   = duplicate_pipe_fd,
		                                                    .create_pipe = create_failed_pipe};
		auto                       failed_create =
		    howdy::pam::detail::create_internal_pipe(O_CLOEXEC, &failed_create_operations);
		ok &= expect(!failed_create.valid() && failed_context.create_calls == 1,
		             "create_pipe failure fails closed");
		ok &= expect(recorded_pipe_descriptors_are_closed(failed_context),
		             "create_pipe failure leaves no descriptors to close");

		PipeContext                high_context;
		const InternalFdOperations high_operations{.context     = &high_context,
		                                           .duplicate   = duplicate_pipe_fd,
		                                           .create_pipe = create_high_pipe};
		{
			auto high_pipe = howdy::pam::detail::create_internal_pipe(O_CLOEXEC, &high_operations);
			ok &= expect(high_pipe.valid() && high_pipe.read.get() > STDERR_FILENO &&
			                 high_pipe.write.get() > STDERR_FILENO &&
			                 high_pipe.read.get() != high_pipe.write.get(),
			             "high-descriptor internal pipe succeeds");
			ok &= expect(high_context.duplicate_calls == 0,
			             "high-descriptor internal pipe skips normalization callback");
		}
		ok &= expect(recorded_pipe_descriptors_are_closed(high_context),
		             "successful high-descriptor pipe closes owned endpoints");

		PipeContext                invalid_context;
		const InternalFdOperations invalid_operations{.context     = &invalid_context,
		                                              .duplicate   = duplicate_pipe_fd,
		                                              .create_pipe = create_invalid_pipe};
		{
			auto invalid_pipe =
			    howdy::pam::detail::create_internal_pipe(O_CLOEXEC, &invalid_operations);
			ok &= expect(!invalid_pipe.valid(), "invalid final pipe endpoint fails closed");
		}
		ok &= expect(recorded_pipe_descriptors_are_closed(invalid_context),
		             "invalid final pipe endpoint does not leak");

		PipeContext                identical_context;
		const InternalFdOperations identical_operations{.context     = &identical_context,
		                                                .duplicate   = duplicate_pipe_fd,
		                                                .create_pipe = create_identical_pipe};
		{
			auto identical_pipe =
			    howdy::pam::detail::create_internal_pipe(O_CLOEXEC, &identical_operations);
			ok &= expect(!identical_pipe.valid(), "identical final pipe endpoints fail closed");
		}
		return ok && expect(recorded_pipe_descriptors_are_closed(identical_context),
		                    "identical final pipe endpoints do not leak");
	}

	auto test_create_internal_pipe_low_and_normalization_failures() -> bool {
		if (!close_standard_descriptors()) {
			return false;
		}

		PipeContext                success_context;
		const InternalFdOperations success_operations{.context     = &success_context,
		                                              .duplicate   = duplicate_pipe_fd,
		                                              .create_pipe = create_raw_pipe};
		{
			auto pipe = howdy::pam::detail::create_internal_pipe(O_CLOEXEC, &success_operations);
			if (!pipe.valid() || pipe.read.get() <= STDERR_FILENO ||
			    pipe.write.get() <= STDERR_FILENO || pipe.read.get() == pipe.write.get()) {
				return false;
			}
		}
		if (success_context.duplicate_calls != 2 ||
		    success_context.duplicate_requests !=
		        std::vector<std::pair<int, int>>{{STDIN_FILENO, STDERR_FILENO + 1},
		                                         {STDOUT_FILENO, STDERR_FILENO + 1}} ||
		    !recorded_pipe_descriptors_are_closed(success_context)) {
			return false;
		}

		for (const std::size_t failure_call : {std::size_t{1}, std::size_t{2}}) {
			PipeContext failure_context;
			failure_context.fail_duplicate_call = failure_call;
			const InternalFdOperations failure_operations{.context     = &failure_context,
			                                              .duplicate   = duplicate_pipe_fd,
			                                              .create_pipe = create_raw_pipe};
			{
				auto pipe =
				    howdy::pam::detail::create_internal_pipe(O_CLOEXEC, &failure_operations);
				if (pipe.valid() || failure_context.duplicate_calls != failure_call) {
					return false;
				}
			}
			if (!recorded_pipe_descriptors_are_closed(failure_context)) {
				return false;
			}
		}
		return true;
	}
}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= test_scoped_fd_lifecycle();
	ok &= test_internal_pipe_validity();
	ok &= run_isolated(test_internal_pipe_low_and_identical_endpoints,
	                   "low and identical InternalPipe endpoints");
	ok &= test_normalize_high_and_invalid();
	ok &= run_isolated(test_normalize_low_and_failures, "low and failing descriptor normalization");
	ok &= run_isolated(test_production_internal_pipe, "production internal pipe operations");
	ok &= test_create_internal_pipe_paths();
	ok &= run_isolated(test_create_internal_pipe_low_and_normalization_failures,
	                   "low and failing internal pipe creation");
	return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
