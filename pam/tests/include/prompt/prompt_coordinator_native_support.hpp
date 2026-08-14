#pragma once

#include "prompt/prompt_coordinator_fake.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <security/pam_appl.h>

namespace howdy::test::prompt_coordinator {

	class ScopedFd {
	public:
		ScopedFd() = default;

		explicit ScopedFd(int fd)
		    : fd_(fd) {}

		ScopedFd(const ScopedFd &)                     = delete;
		auto operator=(const ScopedFd &) -> ScopedFd & = delete;

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

	private:
		int fd_ = -1;
	};

	inline auto original_conversation(int num_msg, const struct pam_message **messages,
	                                  struct pam_response **response, void *appdata_ptr) -> int {
		(void)num_msg;
		(void)messages;
		if (response != nullptr) {
			*response = nullptr;
		}
		if (appdata_ptr != nullptr) {
			++static_cast<FakeContext *>(appdata_ptr)->original_conversation_calls;
		}
		return PAM_CONV_ERR;
	}

	class NativePamFixture {
	public:
		explicit NativePamFixture(FakeContext *context)
		    : context_(context)
		    , original_conv_{.conv = original_conversation, .appdata_ptr = context} {}

		NativePamFixture(const NativePamFixture &)                     = delete;
		auto operator=(const NativePamFixture &) -> NativePamFixture & = delete;

		~NativePamFixture() {
			if (pamh_ != nullptr) {
				pam_end(pamh_, PAM_SUCCESS);
			}
		}

		auto start(bool with_tty) -> bool {
			if (pam_start("howdy-prompt-coordinator-test", "test-user", &original_conv_, &pamh_) !=
			    PAM_SUCCESS) {
				return false;
			}
			if (!with_tty) {
				return true;
			}

			master_fd_.reset(posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC));
			if (master_fd_.get() < 0 || grantpt(master_fd_.get()) != 0 ||
			    unlockpt(master_fd_.get()) != 0) {
				return false;
			}
			char *slave_path = ptsname(master_fd_.get());
			if (slave_path == nullptr || pam_set_item(pamh_, PAM_TTY, slave_path) != PAM_SUCCESS) {
				return false;
			}
			context_->prompt_master_fd = master_fd_.get();
			return true;
		}

		[[nodiscard]] auto pamh() const -> pam_handle_t * {
			return pamh_;
		}

		[[nodiscard]] auto original_conversation_restored() const -> bool {
			const void *item = nullptr;
			if (pam_get_item(pamh_, PAM_CONV, &item) != PAM_SUCCESS || item == nullptr) {
				return false;
			}
			const auto *conversation = static_cast<const struct pam_conv *>(item);
			return conversation->conv == original_conv_.conv &&
			       conversation->appdata_ptr == original_conv_.appdata_ptr;
		}

	private:
		FakeContext    *context_ = nullptr;
		struct pam_conv original_conv_{};
		pam_handle_t   *pamh_ = nullptr;
		ScopedFd        master_fd_;
	};

}  // namespace howdy::test::prompt_coordinator
