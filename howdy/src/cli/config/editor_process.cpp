#include "cli/config/edit_session.hpp"
#include "internal.hpp"
#include "support/eintr.hpp"
#include "support/invoking_user.hpp"
#include "support/invoking_user_env.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <grp.h>
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
			std::string           editor_arg = editor;
			std::string           path_arg   = temp_path.string();
			std::array<char *, 3> exec_argv  = {editor_arg.data(), path_arg.data(), nullptr};

			// execvp may invoke /bin/sh for ENOEXEC. Search with execv so editor execution stays
			// direct.
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

				std::string candidate;
				if (directory.empty()) {
					candidate = editor;
				} else {
					candidate.reserve(directory.size() + 1 + editor.size());
					candidate.append(directory);
					candidate.push_back('/');
					candidate.append(editor);
				}

				execv(candidate.c_str(), exec_argv.data());
				const int error = errno;
				if (!IsEditorUnavailableError(error)) {
					return error;
				}
				if (error == EACCES) {
					last_error = EACCES;
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
				if (initgroups(invoking_user->name.c_str(), invoking_user->gid) != 0 ||
				    setgid(invoking_user->gid) != 0 || setuid(invoking_user->uid) != 0) {
					ReportEditorLaunch(report_fd, EditorLaunchReport::kLaunchFailed, 126);
				}
				if (getuid() != invoking_user->uid || geteuid() != invoking_user->uid ||
				    getgid() != invoking_user->gid || getegid() != invoking_user->gid) {
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

	}  // namespace

	auto RunEditor(const std::string &editor_preference, const std::filesystem::path &temp_path,
	               const std::optional<howdy::native::InvokingUser> &invoking_user) -> int {
		std::array<int, 2> report_pipe{};
		if (pipe2(report_pipe.data(), O_CLOEXEC) != 0) {
			return -1;
		}
		const int report_read_fd  = report_pipe.front();
		const int report_write_fd = report_pipe.back();

		const pid_t child_pid = fork();
		if (child_pid < 0) {
			close(report_read_fd);
			close(report_write_fd);
			return -1;
		}

		if (child_pid == 0) {
			close(report_read_fd);
			RunEditorChild(editor_preference, temp_path, invoking_user, report_write_fd);
		}

		close(report_write_fd);
		int status = 0;
		if (howdy::native::RetryOnEintr([&] -> pid_t {
			    return waitpid(child_pid, &status, 0);
		    }) < 0) {
			close(report_read_fd);
			return -1;
		}

		unsigned char report_byte = 0;
		const ssize_t read_count  = howdy::native::RetryOnEintr([&] -> ssize_t {
			return read(report_read_fd, &report_byte, sizeof(report_byte));
		});
		close(report_read_fd);
		if (read_count < 0) {
			return -1;
		}
		if (read_count == 1) {
			switch (static_cast<EditorLaunchReport>(report_byte)) {
				case EditorLaunchReport::kUnavailable:
					return kEditorUnavailableRunResult;
				case EditorLaunchReport::kLaunchFailed:
					return -1;
			}
			return -1;
		}
		return status;
	}

}  // namespace howdy::native::config_internal
