#include "cli/config_cli.hpp"
#include "common/invoking_user.hpp"
#include "common/invoking_user_env.hpp"
#include "config/config_reader.hpp"
#include "config/config_utils.hpp"
#include "config/config_validation.hpp"
#include "config/runtime_paths.hpp"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <grp.h>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

#include <sys/stat.h>
#include <sys/wait.h>

namespace {

    namespace fs = std::filesystem;

    constexpr int kExitOk    = 0;
    constexpr int kExitAbort = 1;

    auto is_safe_editor_path(const fs::path &path) -> bool {
        return path.is_absolute() && fs::is_regular_file(path) && access(path.c_str(), X_OK) == 0;
    }

    auto resolve_editor(bool allow_env_editor) -> std::string {
        if (allow_env_editor) {
            if (const char *editor = std::getenv("EDITOR");
                editor != nullptr && editor[0] != '\0') {
                const fs::path editor_path(editor);
                if (is_safe_editor_path(editor_path)) {
                    return editor_path.string();
                }
            }
        }

        for (const char *candidate : {"/usr/bin/micro", "/usr/bin/nano", "/usr/bin/vi"}) {
            if (access(candidate, X_OK) == 0) {
                return candidate;
            }
        }

        return {};
    }

    void remove_if_exists(const fs::path &path) {
        std::error_code ec;
        fs::remove(path, ec);
    }

    void reset_editor_environment(const howdy::native::InvokingUser &invoking_user) {
        howdy::native::reset_invoking_user_environment(invoking_user);
    }

    auto create_temp_copy(const fs::path                                   &source_path,
                          const std::optional<howdy::native::InvokingUser> &invoking_user)
        -> std::optional<fs::path> {
        std::ifstream input(source_path, std::ios::binary);
        if (!input.is_open()) {
            return std::nullopt;
        }

        fs::path          temp_dir      = fs::temp_directory_path();
        std::string       temp_template = (temp_dir / "howdy-config-XXXXXX").string();
        std::vector<char> writable(temp_template.begin(), temp_template.end());
        writable.push_back('\0');

        const int fd = mkstemp(writable.data());
        if (fd < 0) {
            return std::nullopt;
        }

        fs::path temp_path(writable.data());
        bool     ok = true;

        if (fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
            ok = false;
        }

        if (ok && invoking_user.has_value() &&
            fchown(fd, invoking_user->uid, invoking_user->gid) != 0) {
            ok = false;
        }

        if (ok) {
            std::array<char, 8192> buffer{};
            while (input.good()) {
                input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const auto bytes_read = input.gcount();
                if (bytes_read <= 0) {
                    continue;
                }

                const char *cursor    = buffer.data();
                auto        remaining = static_cast<std::size_t>(bytes_read);
                while (remaining > 0) {
                    const auto written = write(fd, cursor, remaining);
                    if (written < 0) {
                        if (errno == EINTR) {
                            continue;
                        }
                        ok = false;
                        break;
                    }
                    cursor += written;
                    remaining -= static_cast<std::size_t>(written);
                }

                if (!ok) {
                    break;
                }
            }
        }

        if (ok && fsync(fd) != 0) {
            ok = false;
        }

        close(fd);

        if (!ok || (!input.good() && !input.eof())) {
            remove_if_exists(temp_path);
            return std::nullopt;
        }

        return temp_path;
    }

    auto run_editor(const std::string &editor, const fs::path &temp_path,
                    const std::optional<howdy::native::InvokingUser> &invoking_user) -> int {
        const pid_t child_pid = fork();
        if (child_pid < 0) {
            return -1;
        }

        if (child_pid == 0) {
            if (invoking_user.has_value()) {
                if (initgroups(invoking_user->name.c_str(), invoking_user->gid) != 0 ||
                    setgid(invoking_user->gid) != 0 || setuid(invoking_user->uid) != 0) {
                    _exit(126);
                }
                reset_editor_environment(*invoking_user);
            }

            std::array<char *, 3> exec_argv = {
                const_cast<char *>(editor.c_str()),
                const_cast<char *>(temp_path.c_str()),
                nullptr,
            };
            execv(editor.c_str(), exec_argv.data());
            _exit(127);
        }

        int status = 0;
        while (waitpid(child_pid, &status, 0) < 0) {
            if (errno != EINTR) {
                return -1;
            }
        }
        return status;
    }

    auto read_file_contents_from_fd(int input_fd, std::string *content) -> bool {
        if (content == nullptr) {
            return false;
        }
        content->clear();

        std::array<char, 8192> buffer{};
        while (true) {
            const auto bytes_read = read(input_fd, buffer.data(), buffer.size());
            if (bytes_read == 0) {
                return true;
            }
            if (bytes_read < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }

            content->append(buffer.data(), static_cast<std::size_t>(bytes_read));
        }
    }

    auto write_file_contents_to_fd(int output_fd, const std::string &content) -> bool {
        const char *cursor    = content.data();
        auto        remaining = content.size();
        while (remaining > 0) {
            const auto bytes_written = write(output_fd, cursor, remaining);
            if (bytes_written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            if (bytes_written == 0) {
                return false;
            }
            cursor += bytes_written;
            remaining -= static_cast<std::size_t>(bytes_written);
        }
        return true;
    }

    auto read_temp_config_snapshot(const fs::path &temp_path, std::string *content) -> bool {
        const int input_fd = open(temp_path.c_str(), O_RDONLY | O_NOFOLLOW);
        if (input_fd < 0) {
            return false;
        }

        struct stat edited_stat{};
        const bool  edited_ok = fstat(input_fd, &edited_stat) == 0 && S_ISREG(edited_stat.st_mode);
        if (!edited_ok) {
            close(input_fd);
            return false;
        }

        const bool ok = read_file_contents_from_fd(input_fd, content);
        close(input_fd);
        return ok;
    }

    auto validate_edited_config_content(const std::string &edited_content,
                                        const fs::path &display_path, std::string *error_message)
        -> bool {
        std::string temp_template =
            (fs::temp_directory_path() / "howdy-config-validate-XXXXXX").string();
        std::vector<char> writable(temp_template.begin(), temp_template.end());
        writable.push_back('\0');

        const int fd = mkstemp(writable.data());
        if (fd < 0) {
            return false;
        }

        const fs::path validation_path(writable.data());

        struct ValidationTempCleanup {
            fs::path path;

            ~ValidationTempCleanup() {
                remove_if_exists(path);
            }
        } cleanup{validation_path};

        bool ok = true;
        if (fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
            ok = false;
        }

        if (ok && !write_file_contents_to_fd(fd, edited_content)) {
            ok = false;
        }
        if (ok && fsync(fd) != 0) {
            ok = false;
        }
        if (close(fd) != 0) {
            ok = false;
        }

        if (!ok) {
            return false;
        }

        howdy::native::ConfigReader edited_config(validation_path.string());
        if (!edited_config.ok()) {
            if (error_message != nullptr) {
                *error_message =
                    "Edited config is invalid and was not installed: " + display_path.string();
            }
            return false;
        }

        if (const auto validation = howdy::native::validate_runtime_config(edited_config)) {
            if (error_message != nullptr) {
                *error_message = *validation;
            }
            return false;
        }

        return true;
    }

    auto file_content_matches(const fs::path &path, const std::string &expected) -> bool {
        std::ifstream input(path, std::ios::binary);
        if (!input.is_open()) {
            return false;
        }

        std::string current;
        current.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        return input.good() || input.eof() ? current == expected : false;
    }

    auto replace_config_from_content(const fs::path &config_path, const std::string &edited_content)
        -> bool {
        struct stat current_stat{};
        if (stat(config_path.c_str(), &current_stat) != 0) {
            return false;
        }

        std::string output_template = (config_path.parent_path() / ".howdy-config-XXXXXX").string();
        std::vector<char> writable(output_template.begin(), output_template.end());
        writable.push_back('\0');

        const int output_fd = mkstemp(writable.data());
        if (output_fd < 0) {
            return false;
        }

        const fs::path staged_path(writable.data());
        bool           ok = true;
        if (fchmod(output_fd, current_stat.st_mode & 07777) != 0 ||
            fchown(output_fd, current_stat.st_uid, current_stat.st_gid) != 0) {
            ok = false;
        }

        if (ok && !write_file_contents_to_fd(output_fd, edited_content)) {
            ok = false;
        }

        if (ok && fsync(output_fd) != 0) {
            ok = false;
        }

        if (close(output_fd) != 0) {
            ok = false;
        }

        if (!ok) {
            remove_if_exists(staged_path);
            return false;
        }

        std::error_code ec;
        fs::rename(staged_path, config_path, ec);
        if (ec) {
            remove_if_exists(staged_path);
            return false;
        }

        const int dir_fd = open(config_path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
        if (dir_fd >= 0) {
            fsync(dir_fd);
            close(dir_fd);
        }

        return true;
    }

}  // namespace

int config_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    const auto invoking_user = howdy::native::resolve_invoking_user();
    const auto editor        = resolve_editor(invoking_user.has_value());
    if (editor.empty()) {
        std::cout << "Error: Could not find a suitable text editor.\n";
        std::cout
            << "Set EDITOR to an absolute executable path, or install one of: micro, nano, vi.\n";
        return kExitAbort;
    }

    const auto config_path     = howdy::native::resolve_config_path();
    const auto config_security = howdy::native::check_secure_config_path(config_path);
    if (!config_security.ok) {
        std::cout << config_security.error_message << "\n";
        return kExitAbort;
    }
    const auto temp_path = create_temp_copy(config_path, invoking_user);
    if (!temp_path) {
        std::cout << "Failed to prepare a temporary config copy\n";
        return kExitAbort;
    }

    std::cout << "Editing config.ini in " << fs::path(editor).filename().string() << "\n";

    const int status = run_editor(editor, *temp_path, invoking_user);
    if (status < 0) {
        remove_if_exists(*temp_path);
        std::cout << "Failed to launch editor\n";
        return kExitAbort;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        remove_if_exists(*temp_path);
        std::cout << "Editor exited unsuccessfully; config not updated\n";
        return kExitAbort;
    }

    std::string edited_content;
    if (!read_temp_config_snapshot(*temp_path, &edited_content)) {
        remove_if_exists(*temp_path);
        std::cout << "Failed to install edited config\n";
        return kExitAbort;
    }

    std::string validation_error;
    if (!validate_edited_config_content(edited_content, *temp_path, &validation_error)) {
        if (!validation_error.empty()) {
            std::cout << validation_error << "\n";
        } else {
            std::cout << "Failed to install edited config\n";
        }
        return kExitAbort;
    }

    if (file_content_matches(config_path, edited_content)) {
        remove_if_exists(*temp_path);
        std::cout << "No config changes made\n";
        return kExitOk;
    }

    if (!replace_config_from_content(config_path, edited_content)) {
        remove_if_exists(*temp_path);
        std::cout << "Failed to install edited config\n";
        return kExitAbort;
    }

    remove_if_exists(*temp_path);
    std::cout << "Config updated\n";
    return kExitOk;
}
