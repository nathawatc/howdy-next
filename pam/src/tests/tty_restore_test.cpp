#include "test_support.hpp"
#include "tty_restore.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <string_view>
#include <termios.h>
#include <unistd.h>

#include <security/pam_appl.h>

namespace {

	using howdy::test::expect;

	class ScopedFd {
	public:
		ScopedFd() = default;

		explicit ScopedFd(int fd)
		    : fd_(fd) {}

		ScopedFd(const ScopedFd &)                     = delete;
		auto operator=(const ScopedFd &) -> ScopedFd & = delete;

		ScopedFd(ScopedFd &&other) noexcept
		    : fd_(other.release()) {}

		auto operator=(ScopedFd &&other) noexcept -> ScopedFd & {
			if (this != &other) {
				reset(other.release());
			}
			return *this;
		}

		~ScopedFd() {
			reset();
		}

		[[nodiscard]] auto get() const -> int {
			return fd_;
		}

		void reset(int fd = -1) {
			if (fd_ >= 0) {
				close(fd_);
			}
			fd_ = fd;
		}

		auto release() -> int {
			const int fd = fd_;
			fd_          = -1;
			return fd;
		}

	private:
		int fd_ = -1;
	};

	struct PtyPairOutputs {
		ScopedFd    *master_fd  = nullptr;
		ScopedFd    *slave_fd   = nullptr;
		std::string *slave_name = nullptr;
	};

	auto open_pty_pair(PtyPairOutputs outputs) -> bool {
		const int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
		if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
			if (master >= 0) {
				close(master);
			}
			return false;
		}

		char *name = ptsname(master);
		if (name == nullptr) {
			close(master);
			return false;
		}

		const int slave = open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
		if (slave < 0) {
			close(master);
			return false;
		}

		*outputs.master_fd  = ScopedFd(master);
		*outputs.slave_fd   = ScopedFd(slave);
		*outputs.slave_name = name;
		return true;
	}

	auto test_conv(int /*num_msg*/, const struct pam_message ** /*msgm*/,
	               struct pam_response **response, void * /*appdata_ptr*/) -> int {
		if (response != nullptr) {
			*response = nullptr;
		}
		return PAM_SUCCESS;
	}

	auto read_newline(int fd) -> bool {
		struct pollfd poll_fd{.fd = fd, .events = POLLIN, .revents = 0};
		if (poll(&poll_fd, 1, 1000) <= 0 || (poll_fd.revents & POLLIN) == 0) {
			return false;
		}

		std::array<char, 8> buffer{};
		const ssize_t       bytes_read = read(fd, buffer.data(), buffer.size());
		for (ssize_t index = 0; index < bytes_read; ++index) {
			if (buffer[static_cast<std::size_t>(index)] == '\n') {
				return true;
			}
		}
		return false;
	}

	auto create_regular_file(std::string *path) -> bool {
		constexpr std::string_view kTemplatePath = "/tmp/howdy-tty-restore-test-XXXXXX";
		std::array<char, kTemplatePath.size() + 1> template_path{};
		std::ranges::copy(kTemplatePath, template_path.begin());

		const int fd = mkstemp(template_path.data());
		if (fd < 0) {
			return false;
		}
		close(fd);
		*path = template_path.data();
		return true;
	}

}  // namespace

auto main() -> int {
	bool        ok = true;
	ScopedFd    master_fd;
	ScopedFd    slave_fd;
	std::string slave_name;

	ok &= expect(
	    open_pty_pair({.master_fd = &master_fd, .slave_fd = &slave_fd, .slave_name = &slave_name}),
	    "opens pseudo terminal");
	if (!ok) {
		return 1;
	}

	struct pam_conv conversation{
	    .conv        = test_conv,
	    .appdata_ptr = nullptr,
	};
	pam_handle_t *pamh = nullptr;
	ok &= expect(pam_start("howdy-tty-restore-test", "test-user", &conversation, &pamh) ==
	                 PAM_SUCCESS,
	             "starts PAM handle");
	ok &= expect(pamh != nullptr, "PAM handle is available");
	ok &=
	    expect(pam_set_item(pamh, PAM_TTY, slave_name.c_str()) == PAM_SUCCESS, "sets PAM terminal");
	if (!ok) {
		if (pamh != nullptr) {
			pam_end(pamh, PAM_SYSTEM_ERR);
		}
		return 1;
	}

	struct termios original{};
	ok &= expect(tcgetattr(slave_fd.get(), &original) == 0, "reads original terminal state");
	original.c_lflag |= ECHO;
	ok &= expect(tcsetattr(slave_fd.get(), TCSANOW, &original) == 0, "enables original echo");

	TtyRestoreContext context(pamh);
	ok &= expect(context.can_restore(), "valid context can restore");
	ok &= expect(context.restore_echo(), "enabled echo needs no restoration");

	struct termios hidden = original;
	hidden.c_lflag &= static_cast<tcflag_t>(~ECHO);
	ok &= expect(tcsetattr(slave_fd.get(), TCSANOW, &hidden) == 0, "disables terminal echo");
	ok &= expect(context.restore_echo(), "restores disabled echo");

	struct termios restored{};
	ok &= expect(tcgetattr(slave_fd.get(), &restored) == 0, "reads restored terminal state");
	ok &= expect((restored.c_lflag & ECHO) != 0, "echo is restored");
	ok &= expect(context.write_newline(), "writes terminal newline");
	ok &= expect(read_newline(master_fd.get()), "newline reaches terminal output");

	TtyRestoreContext moved(std::move(context));
	ok &= expect(moved.can_restore(), "move construction transfers context");
	auto *moved_alias = &moved;
	moved             = std::move(*moved_alias);
	ok &= expect(moved.can_restore(), "self move assignment preserves context");

	TtyRestoreContext assigned(pamh);
	assigned = std::move(moved);
	ok &= expect(assigned.can_restore(), "move assignment transfers context");

	std::string regular_path;
	ok &= expect(create_regular_file(&regular_path), "creates regular file tty candidate");

	pam_handle_t *invalid_pamh = nullptr;
	if (!regular_path.empty()) {
		ok &= expect(pam_start("howdy-tty-restore-test", "test-user", &conversation,
		                       &invalid_pamh) == PAM_SUCCESS,
		             "starts PAM handle for invalid terminal");
		ok &= expect(invalid_pamh != nullptr, "invalid PAM handle is available");
		if (invalid_pamh != nullptr) {
			const bool tty_set =
			    pam_set_item(invalid_pamh, PAM_TTY, regular_path.c_str()) == PAM_SUCCESS;
			ok &= expect(tty_set, "sets regular file as PAM terminal");
			if (tty_set) {
				TtyRestoreContext invalid_context(invalid_pamh);
				ok &=
				    expect(!invalid_context.can_restore(), "regular file terminal cannot restore");
				ok &= expect(invalid_context.restore_echo(), "invalid context restore is harmless");
				ok &=
				    expect(invalid_context.write_newline(), "invalid context newline is harmless");
			}
			pam_end(invalid_pamh, ok ? PAM_SUCCESS : PAM_SYSTEM_ERR);
		}
		unlink(regular_path.c_str());
	}

	pam_end(pamh, ok ? PAM_SUCCESS : PAM_SYSTEM_ERR);
	return ok ? 0 : 1;
}
