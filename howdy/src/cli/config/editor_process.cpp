#include "cli/config/edit_session.hpp"
#include "cli/privilege_drop.hpp"
#include "internal.hpp"
#include "support/eintr.hpp"
#include "support/invoking_user.hpp"
#include "support/invoking_user_env.hpp"
#include "support/scoped_fd.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/wait.h>

namespace howdy::native::config_internal {

	namespace {

		enum class EditorLaunchReport : unsigned char {
			kUnavailable = 1,
			kLaunchFailed,
		};

		auto IsEditorUnavailableError(int error) -> bool {
			switch (error) {
				case EACCES:
				case EISDIR:
				case ELOOP:
				case ENAMETOOLONG:
				case ENOENT:
				case ENOEXEC:
				case ENOTDIR:
					return true;
				default:
					return false;
			}
		}

		auto ExecEditorThroughPath(const std::string           &editor,
		                           const std::filesystem::path &temp_path) -> int {
			if (!IsSupportedEditorPreference(editor)) {
				return ENOENT;
			}
			std::string           editor_arg = editor;
			std::string           path_arg   = temp_path.string();
			std::array<char *, 3> exec_argv  = {editor_arg.data(), path_arg.data(), nullptr};

			// execvp may invoke /bin/sh for ENOEXEC. Search with execv so editor execution stays
			// direct. IsSupportedEditorPreference guarantees any path with '/' is absolute.
			if (editor.contains('/')) {
				execv(editor.c_str(), exec_argv.data());
				return errno;
			}

			std::vector<char> default_path;
			std::string_view  search_path;
			if (const char *path = std::getenv("PATH"); path != nullptr) {
				search_path = path;
			} else {
				const auto path_size = confstr(_CS_PATH, nullptr, 0);
				if (path_size == 0) {
					return ENOENT;
				}
				default_path.resize(path_size);
				const auto written = confstr(_CS_PATH, default_path.data(), default_path.size());
				if (written == 0 || written > default_path.size()) {
					return ENOENT;
				}
				search_path = std::string_view(default_path.data(), written - 1);
			}

			int         last_error = ENOENT;
			std::size_t start      = 0;
			while (true) {
				const auto end       = search_path.find(':', start);
				const auto directory = search_path.substr(
				    start, end == std::string_view::npos ? std::string_view::npos : end - start);

				// Ignore empty and relative PATH segments so an editor is never executed from the
				// current working directory (e.g. PATH="" or leading/trailing ':').
				if (!directory.empty() && directory.starts_with('/')) {
					std::string candidate;
					candidate.reserve(directory.size() + 1 + editor.size());
					candidate.append(directory);
					candidate.push_back('/');
					candidate.append(editor);

					execv(candidate.c_str(), exec_argv.data());
					const int error = errno;
					if (!IsEditorUnavailableError(error)) {
						return error;
					}
					if (error == EACCES) {
						last_error = EACCES;
					}
				}

				if (end == std::string_view::npos) {
					break;
				}
				start = end + 1;
			}

			return last_error;
		}

		[[noreturn]] auto ReportEditorLaunch(int report_fd, EditorLaunchReport report,
		                                     int exit_code) -> void {
			const auto                     report_byte = static_cast<unsigned char>(report);
			[[maybe_unused]] const ssize_t written = howdy::native::RetryOnEintr([&] -> ssize_t {
				return write(report_fd, &report_byte, sizeof(report_byte));
			});
			_exit(exit_code);
		}

		auto TryEditor(const std::string &editor, const std::filesystem::path &temp_path,
		               int report_fd) -> void {
			if (!IsEditorUnavailableError(ExecEditorThroughPath(editor, temp_path))) {
				ReportEditorLaunch(report_fd, EditorLaunchReport::kLaunchFailed, 126);
			}
		}

		[[noreturn]] auto
		RunEditorChild(const std::string &editor_preference, const std::filesystem::path &temp_path,
		               const std::optional<howdy::native::InvokingUser> &invoking_user,
		               int                                               report_fd) -> void {
			if (invoking_user.has_value()) {
				if (!howdy::native::cli_internal::DropPrivilegesPermanently(
				        *invoking_user,
				        howdy::native::cli_internal::SystemPrivilegeDropOperations())) {
					ReportEditorLaunch(report_fd, EditorLaunchReport::kLaunchFailed, 126);
				}
				howdy::native::ResetInvokingUserEnvironment(*invoking_user);
			}

			TryEditor(editor_preference, temp_path, report_fd);
			for (const char *fallback : kFallbackEditors) {
				if (editor_preference != fallback) {
					TryEditor(fallback, temp_path, report_fd);
				}
			}
			ReportEditorLaunch(report_fd, EditorLaunchReport::kUnavailable, 127);
		}

		// Signal dispositions while the editor runs. SIGINT and SIGQUIT, which the terminal sends
		// to the whole foreground group, are the editor's to act on, as under system(3): howdy
		// stays to say where the edit is. SIGCHLD gets its default: ignored, which a parent can
		// leave set across exec, it makes the kernel reap the editor, and waitpid has no status to
		// return.
		class EditorSignals {
		public:
			EditorSignals() noexcept
			    : interrupt_(SIGINT, SIG_IGN)
			    , quit_(SIGQUIT, SIG_IGN)
			    , child_(SIGCHLD, SIG_DFL) {}

			// In the editor's process, which keeps the default SIGCHLD.
			void RestoreForEditor() noexcept {
				interrupt_.Restore();
				quit_.Restore();
			}

		private:
			class ScopedDisposition {
			public:
				ScopedDisposition(int signal_number, void (*disposition)(int)) noexcept
				    : signal_number_(signal_number) {
					struct sigaction action{};
					action.sa_handler = disposition;
					changed_          = sigemptyset(&action.sa_mask) == 0 &&
					                    sigaction(signal_number_, &action, &previous_) == 0;
				}

				~ScopedDisposition() {
					Restore();
				}

				ScopedDisposition(const ScopedDisposition &)                     = delete;
				auto operator=(const ScopedDisposition &) -> ScopedDisposition & = delete;
				ScopedDisposition(ScopedDisposition &&)                          = delete;
				auto operator=(ScopedDisposition &&) -> ScopedDisposition &      = delete;

				void Restore() noexcept {
					if (std::exchange(changed_, false)) {
						(void)sigaction(signal_number_, &previous_, nullptr);
					}
				}

			private:
				int              signal_number_ = 0;
				struct sigaction previous_{};
				bool             changed_ = false;
			};

			ScopedDisposition interrupt_;
			ScopedDisposition quit_;
			ScopedDisposition child_;
		};

	}  // namespace

	auto RunEditor(const std::string &editor_preference, const std::filesystem::path &temp_path,
	               const std::optional<howdy::native::InvokingUser> &invoking_user)
	    -> EditorRunResult {
		std::array<int, 2> report_pipe{};
		if (pipe2(report_pipe.data(), O_CLOEXEC) != 0) {
			return EditorRunResult::kLaunchFailed;
		}
		ScopedFd report_reader(report_pipe.front());
		ScopedFd report_writer(report_pipe.back());

		EditorSignals signals;
		const pid_t   child_pid = fork();
		if (child_pid < 0) {
			return EditorRunResult::kLaunchFailed;
		}

		if (child_pid == 0) {
			report_reader.Reset();
			signals.RestoreForEditor();
			try {
				RunEditorChild(editor_preference, temp_path, invoking_user, report_writer.Get());
			} catch (...) {
				// Unwinding further would run the caller's cleanup a second time, in this process.
				ReportEditorLaunch(report_writer.Get(), EditorLaunchReport::kLaunchFailed, 126);
			}
		}

		report_writer.Reset();
		int wait_status = 0;
		if (howdy::native::RetryOnEintr([&] -> pid_t {
			    return waitpid(child_pid, &wait_status, 0);
		    }) < 0) {
			return EditorRunResult::kResultLost;
		}

		unsigned char report_byte = 0;
		const ssize_t read_count  = howdy::native::RetryOnEintr([&] -> ssize_t {
			return read(report_reader.Get(), &report_byte, sizeof(report_byte));
		});
		// A report that cannot be read leaves it open whether an editor ran, and an editor can exit
		// with any code, so the result is lost and the copy is kept.
		if (read_count < 0) {
			return EditorRunResult::kResultLost;
		}
		// The child reports only when it could not become an editor.
		if (read_count == 1) {
			return static_cast<EditorLaunchReport>(report_byte) == EditorLaunchReport::kUnavailable
			           ? EditorRunResult::kUnavailable
					   : EditorRunResult::kLaunchFailed;
		}
		if (WIFEXITED(wait_status)) {
			return WEXITSTATUS(wait_status) == 0 ? EditorRunResult::kSucceeded
			                                     : EditorRunResult::kExitedWithError;
		}
		return EditorRunResult::kKilledBySignal;
	}

}  // namespace howdy::native::config_internal
