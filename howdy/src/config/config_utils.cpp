#include "config/config_utils.hpp"

#include "common/atomic_files.hpp"
#include "common/fd_io.hpp"
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

		struct ConfigLockGuard {
			int fd = -1;

			~ConfigLockGuard() {
				if (fd >= 0) {
					unlock_fd(fd);
					close(fd);
				}
			}
		};

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

		auto join_lines(const std::vector<std::string> &lines) -> std::string {
			std::string content;
			for (const auto &line : lines) {
				content += line;
			}
			return content;
		}

	}  // namespace

	auto is_safe_ini_scalar_value(std::string_view value) -> bool {
		if (!value.empty() && value.front() == '[') {
			return false;
		}

		return std::ranges::all_of(value, [](const char ch) {
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

		const int fd = open(config_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
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
	                        const std::vector<std::string> &lines,
	                        SyncParentDirectoryFn           sync_parent) -> AtomicFileCommitResult {
		auto staged = prepare_staged_file(config_path, ".howdy-config-", kDefaultConfigMode);
		if (!staged.has_value()) {
			return AtomicFileCommitResult::kNotCommitted;
		}

		if (!write_all_to_fd(staged->fd.get(), join_lines(lines))) {
			cleanup_staged_file(*staged);
			return AtomicFileCommitResult::kNotCommitted;
		}
		return install_staged_file(*staged, config_path, sync_parent);
	}

	auto validate_config_content(const std::string &content, std::string *error_message) -> bool {
		const auto        temp_root     = std::filesystem::temp_directory_path();
		std::string       temp_template = (temp_root / "howdy-config-validate-XXXXXX").string();
		std::vector<char> writable(temp_template.begin(), temp_template.end());
		writable.push_back('\0');

		const int fd = mkostemp(writable.data(), O_CLOEXEC);
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

	auto replace_config_content_atomically(const std::filesystem::path &config_path,
	                                       const std::string &content, std::string *error_message,
	                                       bool lock, bool validate_runtime,
	                                       const std::string    *expected_current_content,
	                                       SyncParentDirectoryFn sync_parent) -> bool {
		if (error_message != nullptr) {
			error_message->clear();
		}

		const auto fail = [error_message](const std::string &message) {
			if (error_message != nullptr) {
				*error_message = message;
			}
			return false;
		};

		const auto initial_security = check_secure_config_path(config_path);
		if (!initial_security.ok) {
			return fail(initial_security.error_message);
		}

		ConfigLockGuard config_lock;
		if (lock) {
			config_lock.fd = open_lock_file(config_path);
			if (config_lock.fd < 0 || !lock_fd(config_lock.fd)) {
				if (config_lock.fd >= 0) {
					close(config_lock.fd);
					config_lock.fd = -1;
				}
				return fail("Failed to lock config file");
			}
		}

		if (validate_runtime && !validate_config_content(content, error_message)) {
			return false;
		}

		const auto final_security = check_secure_config_path(config_path);
		if (!final_security.ok) {
			return fail(final_security.error_message);
		}

		struct stat current_stat{};
		if (lstat(config_path.c_str(), &current_stat) != 0 || !S_ISREG(current_stat.st_mode)) {
			return fail("Failed to inspect config file");
		}

		if (expected_current_content != nullptr) {
			const int input_fd = open(config_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
			if (input_fd < 0) {
				return fail("Failed to open config file");
			}

			struct stat opened_stat{};
			const bool  opened_ok =
			    fstat(input_fd, &opened_stat) == 0 && S_ISREG(opened_stat.st_mode);
			const auto current_content = opened_ok ? read_all_from_fd(input_fd) : std::string();
			close(input_fd);
			if (!opened_ok) {
				return fail("Failed to inspect config file");
			}
			if (current_content != *expected_current_content) {
				return fail("Config changed while editing; not installing stale edited config");
			}
		}

		std::string       temp = (config_path.parent_path() / ".howdy-config-XXXXXX").string();
		std::vector<char> writable(temp.begin(), temp.end());
		writable.push_back('\0');

		const int fd = mkostemp(writable.data(), O_CLOEXEC);
		if (fd < 0) {
			return fail("Failed to stage updated config");
		}

		const std::filesystem::path temp_path(writable.data());
		bool                        ok = true;
		if (fchown(fd, current_stat.st_uid, current_stat.st_gid) != 0 ||
		    fchmod(fd, current_stat.st_mode & 07777) != 0) {
			ok = false;
		}
		if (ok && !write_all_to_fd(fd, content)) {
			ok = false;
		}
		if (ok && !sync_fd(fd)) {
			ok = false;
		}
		if (close(fd) != 0) {
			ok = false;
		}

		bool committed = false;
		if (ok) {
			std::error_code ec;
			std::filesystem::rename(temp_path, config_path, ec);
			ok = !ec;
			if (ok) {
				committed = true;
				ok        = sync_parent != nullptr && sync_parent(config_path);
			}
		}

		if (!ok && !committed) {
			std::error_code ec;
			std::filesystem::remove(temp_path, ec);
		}
		if (!ok) {
			if (committed) {
				return fail("Config was installed, but its directory could not be synced; verify "
				            "state before retrying");
			}
			return fail("Failed to install edited config");
		}
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

		const auto initial_security = check_secure_config_path(config_path);
		if (!initial_security.ok) {
			if (error_message != nullptr) {
				*error_message = initial_security.error_message;
			}
			return false;
		}

		ConfigLockGuard config_lock;
		if (lock) {
			config_lock.fd = open_lock_file(config_path);
			if (config_lock.fd < 0 || !lock_fd(config_lock.fd)) {
				if (config_lock.fd >= 0) {
					close(config_lock.fd);
					config_lock.fd = -1;
				}
				if (error_message != nullptr) {
					*error_message = "Failed to lock config file";
				}
				return false;
			}
		}

		const auto security = check_secure_config_path(config_path);
		if (!security.ok) {
			if (error_message != nullptr) {
				*error_message = security.error_message;
			}
			return false;
		}

		const int fd = open(config_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		if (fd < 0) {
			if (error_message != nullptr) {
				*error_message = "Failed to open config file";
			}
			return false;
		}

		const auto current_content = read_all_from_fd(fd);
		close(fd);
		auto lines = split_lines_preserve_newlines(current_content);

		bool updated = false;
		for (auto &line : lines) {
			const auto stripped_pos = line.find_first_not_of(" \t");
			if (stripped_pos == std::string::npos) {
				continue;
			}

			const auto stripped = line.substr(stripped_pos);
			if (stripped.starts_with(key + " =") || stripped.starts_with(key + " ")) {
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
			return false;
		}

		const auto updated_content = join_lines(lines);
		if (validate_runtime && !validate_config_content(updated_content, error_message)) {
			return false;
		}

		std::string install_error;
		const bool  ok = replace_config_content_atomically(
		    config_path, updated_content, error_message == nullptr ? nullptr : &install_error,
		    false, false, &current_content);
		if (!ok && error_message != nullptr) {
			*error_message =
			    install_error.empty() || install_error == "Failed to install edited config"
			        ? "Failed to update config file"
			        : install_error;
		}
		return ok;
	}

}  // namespace howdy::native
