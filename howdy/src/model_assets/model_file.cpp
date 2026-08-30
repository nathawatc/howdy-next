#include "model_assets/model_file.hpp"

#include "support/file_security.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

#include <sys/stat.h>

namespace howdy::native {
	namespace {

		enum class PlaceholderStatus : std::uint8_t {
			kOk,
			kPlaceholder,
			kReadError,
		};

		struct PlaceholderCheckResult {
			PlaceholderStatus status       = PlaceholderStatus::kOk;
			int               error_number = 0;
		};

		// Keep runtime readiness bounded to metadata checks and prefix reads. Full-file
		// SHA-256 integrity validation belongs to download-models, not FaceModel/PAM.
		auto check_placeholder_prefix(const int fd) -> PlaceholderCheckResult {
			std::array<char, 128> buffer{};
			ssize_t               bytes_read = -1;
			do {
				bytes_read = pread(fd, buffer.data(), buffer.size(), 0);
			} while (bytes_read < 0 && errno == EINTR);
			if (bytes_read < 0) {
				const int error_number = errno;
				return {.status = PlaceholderStatus::kReadError, .error_number = error_number};
			}

			std::string prefix(buffer.data(), static_cast<std::size_t>(bytes_read));
			std::ranges::transform(prefix, prefix.begin(),
			                       [](const unsigned char character) -> char {
				                       return static_cast<char>(std::tolower(character));
			                       });
			const auto content_start = prefix.find_first_not_of(" \t\r\n");
			const auto content       = content_start == std::string::npos
			                               ? std::string_view{}
			                               : std::string_view(prefix).substr(content_start);
			if (content.starts_with("version https://git-lfs.github.com/spec/v1") ||
			    content.starts_with("<")) {
				return {.status = PlaceholderStatus::kPlaceholder};
			}
			return {.status = PlaceholderStatus::kOk};
		}

	}  // namespace

	auto check_opencv_model_readiness_with_label(const std::filesystem::path &path,
	                                             const std::string_view       label,
	                                             const std::optional<uid_t>  &owner_uid)
	    -> OpenCvModelReadiness {
		const auto parent = path.parent_path();
		if (parent.empty()) {
			return {.status = OpenCvModelStatus::kInsecure,
			        .error_message =
			            std::string(label) + " must have a parent directory: " + path.string()};
		}
		const auto directory_security =
		    check_secure_root_owned_directory_tree(parent, kModelsDirectoryLabel, owner_uid);
		if (!directory_security.ok) {
			return {.status        = OpenCvModelStatus::kInsecure,
			        .error_message = directory_security.error_message};
		}

		struct stat path_stat{};
		if (lstat(path.c_str(), &path_stat) != 0) {
			if (errno == ENOENT) {
				return {.status        = OpenCvModelStatus::kMissing,
				        .error_message = std::string(label) + " is missing: " + path.string()};
			}
			return {.status        = OpenCvModelStatus::kInsecure,
			        .error_message = "Failed to inspect " + std::string(label) + ": " +
			                         path.string() + " (" + std::strerror(errno) + ")"};
		}
		if (const auto security = check_secure_path_stat(path_stat, SecurePathKind::kRegularFile,
		                                                 path, label, owner_uid);
		    !security.ok) {
			return {.status        = OpenCvModelStatus::kInsecure,
			        .error_message = security.error_message};
		}

		const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		if (fd < 0) {
			return {.status        = OpenCvModelStatus::kInsecure,
			        .error_message = "Failed to open " + std::string(label) + ": " + path.string() +
			                         " (" + std::strerror(errno) + ")"};
		}
		struct stat opened_stat{};
		if (fstat(fd, &opened_stat) != 0) {
			const int error_number = errno;
			close(fd);
			return {.status        = OpenCvModelStatus::kInsecure,
			        .error_message = "Failed to fstat opened " + std::string(label) + " '" +
			                         path.string() + "': " + std::strerror(error_number)};
		}
		if (opened_stat.st_dev != path_stat.st_dev || opened_stat.st_ino != path_stat.st_ino) {
			close(fd);
			return {.status        = OpenCvModelStatus::kInsecure,
			        .error_message = "Model file changed while opening: " + path.string()};
		}
		if (const auto security = check_secure_path_stat(opened_stat, SecurePathKind::kRegularFile,
		                                                 path, label, owner_uid);
		    !security.ok) {
			close(fd);
			return {.status        = OpenCvModelStatus::kInsecure,
			        .error_message = security.error_message};
		}
		if (opened_stat.st_size == 0) {
			close(fd);
			return {.status = OpenCvModelStatus::kInvalid,
			        .error_message =
			            "Model file is empty for " + std::string(label) + ": " + path.string()};
		}

		const auto placeholder = check_placeholder_prefix(fd);
		close(fd);
		if (placeholder.status == PlaceholderStatus::kReadError) {
			return {.status        = OpenCvModelStatus::kInsecure,
			        .error_message = "Failed to pread " + std::string(label) + " '" +
			                         path.string() +
			                         "': " + std::strerror(placeholder.error_number)};
		}
		if (placeholder.status == PlaceholderStatus::kPlaceholder) {
			return {.status        = OpenCvModelStatus::kInvalid,
			        .error_message = "Model file is a placeholder for " + std::string(label) +
			                         ": " + path.string()};
		}
		return {.status = OpenCvModelStatus::kOk, .error_message = {}};
	}

}  // namespace howdy::native
