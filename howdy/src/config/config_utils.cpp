#include "config/config_utils.hpp"

#include "config/config_limits.hpp"
#include "config/config_reader.hpp"
#include "config/config_schema.hpp"
#include "config/config_validation.hpp"
#include "support/atomic_files.hpp"
#include "support/fd_io.hpp"
#include "support/file_lock.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include <sys/file.h>
#include <sys/stat.h>

namespace howdy::native {

	namespace {

		constexpr mode_t kDefaultConfigMode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;

		auto open_lock_file(const std::filesystem::path &config_path) -> int {
			return open(lock_file_path(config_path).c_str(),
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

		auto read_config_from_fd(int fd) -> std::optional<std::string> {
			if (lseek(fd, 0, SEEK_SET) < 0) {
				return std::nullopt;
			}

			const auto result =
			    read_fd_to_string_bounded({.fd = fd, .max_bytes = kMaxConfigFileSize + 1});
			if (result.read_error || result.output.size() > kMaxConfigFileSize) {
				return std::nullopt;
			}
			return result.output;
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

		return std::ranges::all_of(value, [](const char ch) -> bool {
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

		const auto content = read_config_from_fd(fd);
		if (content.has_value()) {
			lines = split_lines_preserve_newlines(*content);
		}
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
		if (content.size() > kMaxConfigFileSize) {
			if (error_message != nullptr) {
				*error_message = "Updated config exceeds maximum size";
			}
			return false;
		}
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

	namespace {
		auto fail_with(std::string *error_message, const std::string &message) -> bool {
			if (error_message != nullptr) {
				*error_message = message;
			}
			return false;
		}

		auto acquire_config_lock(ConfigLockGuard &guard, const std::filesystem::path &config_path)
		    -> bool {
			guard.fd = open_lock_file(config_path);
			if (guard.fd >= 0 && lock_fd(guard.fd)) {
				return true;
			}
			if (guard.fd >= 0) {
				close(guard.fd);
				guard.fd = -1;
			}
			return false;
		}

		auto expected_content_matches(const std::filesystem::path &config_path,
		                              const std::string &expected, std::string *error_message)
		    -> bool {
			const int input_fd = open(config_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
			if (input_fd < 0) {
				return fail_with(error_message, "Failed to open config file");
			}
			struct stat opened_stat{};
			const bool  opened_ok =
			    fstat(input_fd, &opened_stat) == 0 && S_ISREG(opened_stat.st_mode);
			const auto current_content = opened_ok ? read_config_from_fd(input_fd) : std::nullopt;
			close(input_fd);
			if (!opened_ok) {
				return fail_with(error_message, "Failed to inspect config file");
			}
			if (!current_content.has_value()) {
				return fail_with(error_message, "Failed to read config file");
			}
			if (*current_content != expected) {
				return fail_with(
				    error_message,
				    "Config changed while editing; not installing stale edited config");
			}
			return true;
		}

		enum class ConfigInstallResult : std::uint8_t {
			ok,
			stage_failed,
			not_committed,
			committed_not_durable,
		};

		auto install_config_content(const std::filesystem::path &config_path,
		                            const std::string &content, const struct stat &current_stat,
		                            SyncParentDirectoryFn sync_parent) -> ConfigInstallResult {
			std::string       temp = (config_path.parent_path() / ".howdy-config-XXXXXX").string();
			std::vector<char> writable(temp.begin(), temp.end());
			writable.push_back('\0');
			const int fd = mkostemp(writable.data(), O_CLOEXEC);
			if (fd < 0) {
				return ConfigInstallResult::stage_failed;
			}

			const std::filesystem::path temp_path(writable.data());
			bool ok = fchown(fd, current_stat.st_uid, current_stat.st_gid) == 0 &&
			          fchmod(fd, current_stat.st_mode & 07777) == 0 &&
			          write_all_to_fd(fd, content) && sync_fd(fd);
			if (close(fd) != 0) {
				ok = false;
			}
			if (!ok) {
				std::error_code ec;
				std::filesystem::remove(temp_path, ec);
				return ConfigInstallResult::not_committed;
			}

			std::error_code ec;
			std::filesystem::rename(temp_path, config_path, ec);
			if (ec) {
				std::filesystem::remove(temp_path, ec);
				return ConfigInstallResult::not_committed;
			}
			return sync_parent != nullptr && sync_parent(config_path)
			           ? ConfigInstallResult::ok
			           : ConfigInstallResult::committed_not_durable;
		}

		struct ConfigLineReplacement {
			std::string_view   section;
			const std::string &key;
			const std::string &value;
		};

		enum class ConfigLineReplaceResult : std::uint8_t {
			not_found,
			replaced,
			duplicate,
		};

		auto ini_identifier_equal(std::string_view left, std::string_view right) -> bool {
			return left.size() == right.size() &&
			       std::ranges::equal(
			           left, right, [](unsigned char left_char, unsigned char right_char) -> bool {
				           return std::tolower(left_char) == std::tolower(right_char);
			           });
		}

		auto section_name(std::string_view line) -> std::optional<std::string_view> {
			constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";
			if (line.starts_with(kUtf8Bom)) {
				line.remove_prefix(kUtf8Bom.size());
			}
			if (line.empty() || line.front() != '[') {
				return std::nullopt;
			}
			const auto end = line.find(']');
			if (end == std::string_view::npos) {
				return std::nullopt;
			}
			return line.substr(1, end - 1);
		}

		auto assignment_name(std::string_view line) -> std::optional<std::string_view> {
			const auto separator = line.find_first_of("=:");
			if (separator == std::string_view::npos) {
				return std::nullopt;
			}
			auto name = line.substr(0, separator);
			while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
				name.remove_suffix(1);
			}
			return name;
		}

		auto replace_line_value(std::vector<std::string> &lines, ConfigLineReplacement replacement)
		    -> ConfigLineReplaceResult {
			std::string_view current_section;
			bool             has_previous_name = false;
			std::string     *matching_line     = nullptr;
			for (auto &line : lines) {
				const auto stripped_pos = line.find_first_not_of(" \t");
				if (stripped_pos == std::string::npos) {
					continue;
				}
				const std::string_view stripped(line.data() + stripped_pos,
				                                line.size() - stripped_pos);
				if (stripped.front() == ';' || stripped.front() == '#') {
					continue;
				}
				if (has_previous_name && stripped_pos != 0) {
					continue;
				}
				if (const auto section = section_name(stripped)) {
					current_section   = *section;
					has_previous_name = false;
					continue;
				}

				const auto name = assignment_name(stripped);
				if (name.has_value()) {
					has_previous_name = true;
					if (!ini_identifier_equal(current_section, replacement.section) ||
					    !ini_identifier_equal(*name, replacement.key)) {
						continue;
					}
				} else {
					const auto name_end = stripped.find_first_of(" \t");
					if (!ini_identifier_equal(current_section, replacement.section) ||
					    name_end == std::string_view::npos ||
					    !ini_identifier_equal(stripped.substr(0, name_end), replacement.key)) {
						continue;
					}
					has_previous_name = true;
				}

				if (matching_line != nullptr) {
					return ConfigLineReplaceResult::duplicate;
				}
				matching_line = &line;
			}
			if (matching_line == nullptr) {
				return ConfigLineReplaceResult::not_found;
			}
			*matching_line = replacement.key;
			*matching_line += " = ";
			*matching_line += replacement.value;
			*matching_line += '\n';
			return ConfigLineReplaceResult::replaced;
		}
	}  // namespace

	auto replace_config_content_atomically(const std::filesystem::path &config_path,
	                                       const std::string &content, std::string *error_message,
	                                       bool lock, bool validate_runtime,
	                                       const std::string    *expected_current_content,
	                                       SyncParentDirectoryFn sync_parent) -> bool {
		if (error_message != nullptr) {
			error_message->clear();
		}
		if (content.size() > kMaxConfigFileSize) {
			return fail_with(error_message, "Updated config exceeds maximum size");
		}

		const auto initial_security = check_secure_config_path(config_path);
		if (!initial_security.ok) {
			return fail_with(error_message, initial_security.error_message);
		}

		ConfigLockGuard config_lock;
		if (lock && !acquire_config_lock(config_lock, config_path)) {
			return fail_with(error_message, "Failed to lock config file");
		}

		if (validate_runtime && !validate_config_content(content, error_message)) {
			return false;
		}

		const auto final_security = check_secure_config_path(config_path);
		if (!final_security.ok) {
			return fail_with(error_message, final_security.error_message);
		}

		struct stat current_stat{};
		if (lstat(config_path.c_str(), &current_stat) != 0 || !S_ISREG(current_stat.st_mode)) {
			return fail_with(error_message, "Failed to inspect config file");
		}

		if (expected_current_content != nullptr) {
			if (!expected_content_matches(config_path, *expected_current_content, error_message)) {
				return false;
			}
		}
		const auto install_result =
		    install_config_content(config_path, content, current_stat, sync_parent);
		if (install_result == ConfigInstallResult::committed_not_durable) {
			return fail_with(error_message,
			                 "Config was installed, but its directory could not be synced; verify "
			                 "state before retrying");
		}
		if (install_result == ConfigInstallResult::stage_failed) {
			return fail_with(error_message, "Failed to stage updated config");
		}
		if (install_result == ConfigInstallResult::not_committed) {
			return fail_with(error_message, "Failed to install edited config");
		}
		return true;
	}

	auto update_config_value(const std::filesystem::path &config_path, const std::string &key,
	                         std::string *error_message, const std::string &value, bool lock,
	                         bool validate_runtime) -> bool {
		if (!is_safe_ini_scalar_value(value)) {
			return fail_with(error_message,
			                 "Config values must be single-line scalars and cannot start with [");
		}

		const auto initial_security = check_secure_config_path(config_path);
		if (!initial_security.ok) {
			return fail_with(error_message, initial_security.error_message);
		}

		ConfigLockGuard config_lock;
		if (lock && !acquire_config_lock(config_lock, config_path)) {
			return fail_with(error_message, "Failed to lock config file");
		}

		const auto security = check_secure_config_path(config_path);
		if (!security.ok) {
			return fail_with(error_message, security.error_message);
		}

		const int fd = open(config_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		if (fd < 0) {
			return fail_with(error_message, "Failed to open config file");
		}

		const auto current_content = read_config_from_fd(fd);
		close(fd);
		if (!current_content.has_value()) {
			return fail_with(error_message, "Failed to read config file");
		}
		auto lines = split_lines_preserve_newlines(*current_content);

		const auto options = config_schema::runtime_config_options();
		const auto option  = std::ranges::find_if(options, [&](const auto &candidate) -> auto {
			return candidate.key == key;
		});
		if (option == options.end()) {
			return fail_with(error_message,
			                 "Could not find a \"" + key + "\" config option to set");
		}
		const auto replace_result =
		    replace_line_value(lines, {.section = option->section, .key = key, .value = value});
		if (replace_result == ConfigLineReplaceResult::duplicate) {
			return fail_with(error_message, "Config option \"" + key +
			                                    "\" appears more than once in section [" +
			                                    std::string(option->section) + "]");
		}
		if (replace_result == ConfigLineReplaceResult::not_found) {
			return fail_with(error_message,
			                 "Could not find a \"" + key + "\" config option to set");
		}

		const auto updated_content = join_lines(lines);
		if (validate_runtime && !validate_config_content(updated_content, error_message)) {
			return false;
		}

		std::string install_error;
		const bool  ok = replace_config_content_atomically(
		    config_path, updated_content, error_message == nullptr ? nullptr : &install_error,
		    false, false, &*current_content);
		if (!ok && error_message != nullptr) {
			*error_message =
			    install_error.empty() || install_error == "Failed to install edited config"
			        ? "Failed to update config file"
			        : install_error;
		}
		return ok;
	}

}  // namespace howdy::native
