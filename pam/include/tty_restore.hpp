#ifndef HOWDY_PAM_TTY_RESTORE_HH
#define HOWDY_PAM_TTY_RESTORE_HH

#include <string>
#include <termios.h>

struct pam_handle;

class TtyRestoreContext {
public:
	explicit TtyRestoreContext(pam_handle *pamh);
	~TtyRestoreContext();

	TtyRestoreContext(const TtyRestoreContext &)                     = delete;
	auto operator=(const TtyRestoreContext &) -> TtyRestoreContext & = delete;
	TtyRestoreContext(TtyRestoreContext &&other) noexcept;
	auto operator=(TtyRestoreContext &&other) noexcept -> TtyRestoreContext &;

	[[nodiscard]] auto can_restore() const -> bool;
	auto               restore_echo(std::string *error_message = nullptr) const -> bool;
	auto               write_newline(std::string *error_message = nullptr) const -> bool;

private:
	int            fd_ = -1;
	struct termios original_{};
	bool           valid_ = false;
};

#endif  // HOWDY_PAM_TTY_RESTORE_HH
