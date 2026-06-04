#include "config/config_utils.hpp"

#include "config/config_reader.hpp"
#include "config/config_validation.hpp"

#include <algorithm>
#include <array>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include <sys/file.h>
#include <sys/stat.h>

namespace howdy::native {

    namespace {

        constexpr mode_t kDefaultConfigMode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;

        auto lock_path_for_config(const std::filesystem::path &config_path)
            -> std::filesystem::path {
            return config_path.string() + ".lock";
        }

        auto open_lock_file(const std::filesystem::path &config_path) -> int {
            return open(lock_path_for_config(config_path).c_str(),
                        O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
        }

        auto lock_fd(int fd) -> bool {
            while (flock(fd, LOCK_EX) != 0) {
                if (errno != EINTR) {
                    return false;
                }
            }
            return true;
        }

        void unlock_fd(int fd) {
            while (flock(fd, LOCK_UN) != 0 && errno == EINTR) {
            }
        }

        auto read_all_from_fd(int fd) -> std::string {
            if (lseek(fd, 0, SEEK_SET) < 0) {
                return {};
            }

            std::string            content;
            std::array<char, 4096> buffer{};
            while (true) {
                const auto bytes_read = read(fd, buffer.data(), buffer.size());
                if (bytes_read == 0) {
                    return content;
                }
                if (bytes_read < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    return {};
                }
                content.append(buffer.data(), static_cast<std::size_t>(bytes_read));
            }
        }

        auto split_lines_preserve_newlines(const std::string &content) -> std::vector<std::string> {
            std::vector<std::string> lines;
            std::size_t              start = 0;
            while (start < content.size()) {
                const auto end = content.find('\n', start);
                if (end == std::string::npos) {
                    lines.push_back(content.substr(start));
                    break;
                }
                lines.push_back(content.substr(start, (end - start) + 1));
                start = end + 1;
            }
            return lines;
        }

        auto write_all_to_fd(int fd, const std::string &content) -> bool {
            const char *cursor    = content.data();
            std::size_t remaining = content.size();
            while (remaining > 0) {
                const auto bytes_written = write(fd, cursor, remaining);
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

        auto join_lines(const std::vector<std::string> &lines) -> std::string {
            std::string content;
            for (const auto &line : lines) {
                content += line;
            }
            return content;
        }

        auto sync_parent_directory(const std::filesystem::path &path) -> void {
            const int dir_fd = open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
            if (dir_fd >= 0) {
                fsync(dir_fd);
                close(dir_fd);
            }
        }

        auto validate_config_content(const std::string &content, std::string *error_message)
            -> bool {
            const auto        temp_root     = std::filesystem::temp_directory_path();
            std::string       temp_template = (temp_root / "howdy-config-validate-XXXXXX").string();
            std::vector<char> writable(temp_template.begin(), temp_template.end());
            writable.push_back('\0');

            const int fd = mkstemp(writable.data());
            if (fd < 0) {
                if (error_message != nullptr) {
                    *error_message = "Failed to validate updated config";
                }
                return false;
            }

            const std::filesystem::path temp_path(writable.data());
            bool                        ok = write_all_to_fd(fd, content);
            if (close(fd) != 0) {
                ok = false;
            }

            if (!ok) {
                std::error_code ec;
                std::filesystem::remove(temp_path, ec);
                if (error_message != nullptr) {
                    *error_message = "Failed to validate updated config";
                }
                return false;
            }

            ConfigReader    config(temp_path.string());
            std::error_code ec;
            std::filesystem::remove(temp_path, ec);

            if (!config.ok()) {
                if (error_message != nullptr) {
                    *error_message = "Updated config is invalid";
                }
                return false;
            }

            if (const auto validation = validate_runtime_config(config)) {
                if (error_message != nullptr) {
                    *error_message = *validation;
                }
                return false;
            }

            return true;
        }

    }  // namespace

    auto is_safe_ini_scalar_value(std::string_view value) -> bool {
        if (!value.empty() && value.front() == '[') {
            return false;
        }

        return std::all_of(value.begin(), value.end(), [](const char ch) {
            return ch != '\0' && ch != '\n' && ch != '\r';
        });
    }

    auto read_config_lines(const std::filesystem::path &config_path, bool lock)
        -> std::vector<std::string> {
        std::vector<std::string> lines;

        int lock_fd_handle = -1;
        if (lock) {
            lock_fd_handle = open_lock_file(config_path);
            if (lock_fd_handle < 0 || !lock_fd(lock_fd_handle)) {
                if (lock_fd_handle >= 0) {
                    close(lock_fd_handle);
                }
                return lines;
            }
        }

        const int fd = open(config_path.c_str(), O_RDONLY | O_NOFOLLOW);
        if (fd < 0) {
            if (lock_fd_handle >= 0) {
                unlock_fd(lock_fd_handle);
                close(lock_fd_handle);
            }
            return lines;
        }

        const auto security = check_secure_config_path(config_path);
        if (!security.ok) {
            close(fd);
            if (lock_fd_handle >= 0) {
                unlock_fd(lock_fd_handle);
                close(lock_fd_handle);
            }
            return lines;
        }

        lines = split_lines_preserve_newlines(read_all_from_fd(fd));
        close(fd);
        if (lock_fd_handle >= 0) {
            unlock_fd(lock_fd_handle);
            close(lock_fd_handle);
        }
        return lines;
    }

    auto atomic_write_lines(const std::filesystem::path    &config_path,
                            const std::vector<std::string> &lines) -> bool {
        const auto parent = config_path.parent_path();
        std::filesystem::create_directories(parent);

        struct stat current_stat{};
        const bool  have_current_stat = lstat(config_path.c_str(), &current_stat) == 0;
        if (have_current_stat && !S_ISREG(current_stat.st_mode)) {
            return false;
        }

        std::string       temp = (parent / ".howdy-config-XXXXXX").string();
        std::vector<char> writable(temp.begin(), temp.end());
        writable.push_back('\0');

        const int fd = mkstemp(writable.data());
        if (fd < 0) {
            return false;
        }

        const std::filesystem::path temp_path(writable.data());
        bool                        ok = true;
        if (have_current_stat) {
            if (fchmod(fd, current_stat.st_mode & 07777) != 0 ||
                fchown(fd, current_stat.st_uid, current_stat.st_gid) != 0) {
                ok = false;
            }
        } else if (fchmod(fd, kDefaultConfigMode) != 0) {
            ok = false;
        }

        const auto content = join_lines(lines);
        if (ok && !write_all_to_fd(fd, content)) {
            ok = false;
        }

        if (ok && fsync(fd) != 0) {
            ok = false;
        }
        if (close(fd) != 0) {
            ok = false;
        }

        if (!ok) {
            std::error_code ec;
            std::filesystem::remove(temp_path, ec);
            return false;
        }

        std::error_code ec;
        std::filesystem::rename(temp_path, config_path, ec);
        if (ec) {
            std::filesystem::remove(temp_path, ec);
            return false;
        }
        sync_parent_directory(config_path);
        return true;
    }

    auto update_config_value(const std::filesystem::path &config_path, const std::string &key,
                             const std::string &value, std::string *error_message, bool lock,
                             bool validate_runtime) -> bool {
        if (!is_safe_ini_scalar_value(value)) {
            if (error_message != nullptr) {
                *error_message =
                    "Config values must be single-line scalars and cannot start with [";
            }
            return false;
        }

        const auto security = check_secure_config_path(config_path);
        if (!security.ok) {
            if (error_message != nullptr) {
                *error_message = security.error_message;
            }
            return false;
        }

        int lock_fd_handle = -1;
        if (lock) {
            lock_fd_handle = open_lock_file(config_path);
            if (lock_fd_handle < 0 || !lock_fd(lock_fd_handle)) {
                if (lock_fd_handle >= 0) {
                    close(lock_fd_handle);
                }
                if (error_message != nullptr) {
                    *error_message = "Failed to lock config file";
                }
                return false;
            }
        }

        const int fd = open(config_path.c_str(), O_RDONLY | O_NOFOLLOW);
        if (fd < 0) {
            if (lock_fd_handle >= 0) {
                unlock_fd(lock_fd_handle);
                close(lock_fd_handle);
            }
            if (error_message != nullptr) {
                *error_message = "Failed to open config file";
            }
            return false;
        }

        auto lines = split_lines_preserve_newlines(read_all_from_fd(fd));
        close(fd);

        bool updated = false;
        for (auto &line : lines) {
            const auto stripped_pos = line.find_first_not_of(" \t");
            if (stripped_pos == std::string::npos) {
                continue;
            }

            const auto stripped = line.substr(stripped_pos);
            if (stripped.rfind(key + " =", 0) == 0 || stripped.rfind(key + " ", 0) == 0) {
                line = key;
                line += " = ";
                line += value;
                line += "\n";
                updated = true;
                break;
            }
        }

        if (!updated) {
            if (error_message != nullptr) {
                *error_message = "Could not find a \"" + key + "\" config option to set";
            }
            if (lock_fd_handle >= 0) {
                unlock_fd(lock_fd_handle);
                close(lock_fd_handle);
            }
            return false;
        }

        const auto updated_content = join_lines(lines);
        bool       validated       = true;
        if (validate_runtime) {
            validated = validate_config_content(updated_content, error_message);
        }
        const bool ok = validated && atomic_write_lines(config_path, lines);
        if (lock_fd_handle >= 0) {
            unlock_fd(lock_fd_handle);
            close(lock_fd_handle);
        }
        if (!ok && validated && error_message != nullptr && error_message->empty()) {
            *error_message = "Failed to update config file";
        }
        return ok;
    }

}  // namespace howdy::native
