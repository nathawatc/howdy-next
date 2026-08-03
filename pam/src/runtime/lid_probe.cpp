#include "runtime/lid_probe.hpp"

#include <cerrno>
#include <fstream>
#include <glob.h>
#include <string>
#include <system_error>
#include <utility>

namespace howdy::pam::runtime {

	namespace {

		constexpr std::string_view kLidStatePattern = "/proc/acpi/button/lid/*/state";

		struct GlobResultGuard {
			glob_t result{};

			GlobResultGuard()                                            = default;
			GlobResultGuard(const GlobResultGuard &)                     = delete;
			auto operator=(const GlobResultGuard &) -> GlobResultGuard & = delete;
			GlobResultGuard(GlobResultGuard &&)                          = delete;
			auto operator=(GlobResultGuard &&) -> GlobResultGuard &      = delete;

			~GlobResultGuard() noexcept {
				globfree(&result);
			}
		};

		auto glob_error_message(int return_value, int error_number) -> std::string {
			std::string message = "Failed to read files from glob: " + std::to_string(return_value);
			if (error_number != 0) {
				const std::error_code error(error_number, std::generic_category());
				message += "; Underlying error: ";
				message += error.message();
				message += " (" + std::to_string(error_number) + ")";
			}
			return message;
		}

		auto file_error_message(const char *path) -> std::string {
			return "Failed to read lid state file: " + std::string(path == nullptr ? "" : path);
		}

	}  // namespace

	auto read_lid_state() -> LidStateResult {
		return read_lid_state_from_pattern(kLidStatePattern);
	}

	auto read_lid_state_from_pattern(std::string_view pattern) -> LidStateResult {
		if (pattern.empty() || pattern.contains('\0')) {
			return {
			    .status        = LidProbeStatus::kError,
			    .state         = LidState::kUnknown,
			    .error_message = "Invalid lid state pattern",
			};
		}

		const std::string pattern_storage(pattern);
		GlobResultGuard   glob_result;
		errno                  = 0;
		const int return_value = glob(pattern_storage.c_str(), 0, nullptr, &glob_result.result);
		const int error_number = errno;
		if (return_value != 0 && return_value != GLOB_NOMATCH) {
			return {
			    .status        = LidProbeStatus::kError,
			    .state         = LidState::kUnknown,
			    .error_message = glob_error_message(return_value, error_number),
			};
		}

		if (return_value == GLOB_NOMATCH) {
			return {};
		}

		bool        saw_open   = false;
		bool        saw_closed = false;
		std::string read_error;
		for (std::size_t index = 0; index < glob_result.result.gl_pathc; ++index) {
			const char *path = glob_result.result.gl_pathv[index];
			if (path == nullptr) {
				continue;
			}

			std::ifstream file(path);
			std::string   lid_state;
			if (!std::getline(file, lid_state)) {
				if (read_error.empty()) {
					read_error = file_error_message(path);
				}
				continue;
			}

			if (lid_state.contains("closed")) {
				saw_closed = true;
			} else if (lid_state.contains("open")) {
				saw_open = true;
			}
		}

		LidState state = LidState::kUnknown;
		if (saw_closed) {
			state = LidState::kClosed;
		} else if (saw_open) {
			state = LidState::kOpen;
		}
		if (!read_error.empty()) {
			return {
			    .status        = LidProbeStatus::kError,
			    .state         = state,
			    .error_message = std::move(read_error),
			};
		}
		return {.state = state};
	}

}  // namespace howdy::pam::runtime
