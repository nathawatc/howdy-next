#include "cli/config_cli.hpp"
#include "cli/config_cli_test_support.hpp"
#include "cli/config_edit_session.hpp"
#include "config/config_limits.hpp"
#include "config/config_test_hooks.hpp"
#include "test_support.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include <sys/stat.h>

namespace howdy::test::config_cli {

	using howdy::test::expect;
	using howdy::test::read_file;
	using howdy::test::write_file;

	namespace {

		constexpr std::string_view kOriginalContent = "[core]\ndisabled = false\n";

		class ScopedEnvironmentVariable {
		public:
			ScopedEnvironmentVariable(const char *name, const std::string &value)
			    : name_(name) {
				if (const char *current = std::getenv(name); current != nullptr) {
					original_ = current;
				}
				setenv(name_, value.c_str(), 1);
			}

			ScopedEnvironmentVariable(const ScopedEnvironmentVariable &) = delete;
			auto operator=(const ScopedEnvironmentVariable &)
			    -> ScopedEnvironmentVariable & = delete;

			~ScopedEnvironmentVariable() noexcept {
				if (original_.has_value()) {
					setenv(name_, original_->c_str(), 1);
				} else {
					unsetenv(name_);
				}
			}

		private:
			const char                *name_;
			std::optional<std::string> original_;
		};

		auto path_reported_after(const std::string &output, const std::string &prefix)
		    -> std::filesystem::path {
			const auto start = output.find(prefix);
			if (start == std::string::npos) {
				return {};
			}
			const auto path_start = start + prefix.size();
			const auto path_end   = output.find('\n', path_start);
			return output.substr(path_start, path_end - path_start);
		}

		auto public_entrypoint_preserves_invalid_edit() -> bool {
			namespace fs = std::filesystem;

			const std::string temp_template =
			    (fs::current_path() / "howdy-config-cli-integration-XXXXXX").string();
			std::vector<char> writable_template(temp_template.begin(), temp_template.end());
			writable_template.push_back('\0');
			const char *created_path = mkdtemp(writable_template.data());
			if (!expect(created_path != nullptr, "integration creates temp directory")) {
				return false;
			}

			bool            ok          = true;
			const fs::path  temp_root   = created_path;
			const fs::path  config_path = temp_root / "config.ini";
			const fs::path  editor_path = temp_root / "fake-editor";
			std::error_code error;
			ok &= expect(write_file(config_path, std::string{kOriginalContent}),
			             "integration writes config baseline");
			ok &= expect(chmod(config_path.c_str(), 0600) == 0, "integration secures config file");
			ok &= expect(write_file(editor_path, "#!/bin/sh\nprintf '[core\\n' > \"$1\"\n"),
			             "integration writes fake editor");
			ok &= expect(chmod(editor_path.c_str(), 0700) == 0,
			             "integration makes fake editor executable");

			ScopedEnvironmentVariable editor_env("EDITOR", editor_path.string());
			ScopedEnvironmentVariable config_env("HOWDY_CONFIG", config_path.string());
			ScopedEnvironmentVariable sudo_uid_env("SUDO_UID", std::to_string(getuid()));
			ScopedEnvironmentVariable sudo_gid_env("SUDO_GID", std::to_string(getgid()));

			std::array<char *, 1> argv{const_cast<char *>("howdy-config")};
			std::ostringstream    output;
			int                   exit_code = 0;
			{
				ScopedStreamBuffer stdout_guard(std::cout, output.rdbuf());
				exit_code = config_main(1, argv.data());
			}

			constexpr std::string_view recovery_prefix =
			    "Edited config is invalid and was not installed: ";
			const auto reported_temp_path =
			    path_reported_after(output.str(), std::string(recovery_prefix));
			ok &= expect(exit_code == 1, "integration aborts invalid edit");
			ok &= expect(output.str() == "Editing config.ini in fake-editor\n" +
			                                 std::string(recovery_prefix) +
			                                 reported_temp_path.string() + "\n",
			             "integration output exact");
			ok &= expect(!reported_temp_path.empty(), "integration reports recovery path");
			ok &= expect(fs::exists(reported_temp_path, error) && !error,
			             "integration preserves invalid temp file");
			ok &= expect(read_file(reported_temp_path) == "[core\n",
			             "integration keeps malformed content");
			fs::remove(reported_temp_path, error);
			fs::remove_all(temp_root, error);
			return ok;
		}

		auto production_config_reads_are_bounded() -> bool {
			namespace fs = std::filesystem;

			bool       ok = true;
			const auto dependencies =
			    howdy::native::config_internal::default_config_edit_dependencies();
			const fs::path    source_path = fs::current_path() / "howdy-config-cli-size-test.ini";
			const std::string at_limit(howdy::native::kMaxConfigFileSize, 'x');
			const std::string over_limit(howdy::native::kMaxConfigFileSize + 1, 'x');
			std::error_code   error;

			ok &= expect(write_file(source_path, at_limit), "write source config at size limit");
			const auto copy =
			    dependencies.create_temp_copy(dependencies.context, source_path, std::nullopt);
			ok &= expect(copy.has_value() && copy->original_content == at_limit,
			             "source config exactly at size limit is copied");
			if (copy.has_value()) {
				std::string snapshot;
				ok &= expect(dependencies.read_temp_config_snapshot(dependencies.context,
				                                                    copy->path, &snapshot) &&
				                 snapshot == at_limit,
				             "edited temp config exactly at size limit is read");
				ok &= expect(write_file(copy->path, over_limit),
				             "write oversized edited temp config");
				ok &= expect(!dependencies.read_temp_config_snapshot(dependencies.context,
				                                                     copy->path, &snapshot),
				             "oversized edited temp config is rejected");
				dependencies.remove_if_exists(dependencies.context, copy->path);
			}

			ok &= expect(write_file(source_path, over_limit), "write oversized source config");
			ok &= expect(
			    !dependencies.create_temp_copy(dependencies.context, source_path, std::nullopt),
			    "oversized source config is rejected before temp copy");
			fs::remove(source_path, error);
			return ok;
		}

		auto production_edit_reads_are_descriptor_bound() -> bool {
			namespace fs = std::filesystem;

			const auto temp_template   = fs::current_path() / "howdy-config-cli-identity-XXXXXX";
			const auto template_string = temp_template.string();
			std::vector<char> writable(template_string.begin(), template_string.end());
			writable.push_back('\0');
			const char *created_path = mkdtemp(writable.data());
			if (!expect(created_path != nullptr, "create edit identity temp directory")) {
				return false;
			}

			bool              ok = true;
			const fs::path    temp_root(created_path);
			const fs::path    config_path = temp_root / "config.ini";
			const fs::path    backup_path = temp_root / "config-backup.ini";
			const std::string original    = "[core]\ndisabled = false\n";
			const auto        dependencies =
			    howdy::native::config_internal::default_config_edit_dependencies();
			std::error_code error;

			auto write_secure_config = [&]() -> bool {
				return write_file(config_path, original) && chmod(config_path.c_str(), 0644) == 0;
			};
			auto replace_with_symlink = [&]() -> bool {
				return rename(config_path.c_str(), backup_path.c_str()) == 0 &&
				       symlink("/dev/null", config_path.c_str()) == 0;
			};

			ok &= expect(write_secure_config(), "write edit identity source config");
			{
				bool                                                replacement_ok = false;
				const howdy::native::config_test_hooks::ScopedHooks hooks([&]() -> void {
					replacement_ok = replace_with_symlink();
				});

				const auto copy =
				    dependencies.create_temp_copy(dependencies.context, config_path, std::nullopt);
				ok &= expect(replacement_ok, "create_temp_copy hook replaces source pathname");
				ok &= expect(copy.has_value() && copy->original_content == original,
				             "create_temp_copy consumes opened config descriptor");
				if (copy.has_value()) {
					dependencies.remove_if_exists(dependencies.context, copy->path);
				}
			}
			fs::remove(config_path, error);
			fs::remove(backup_path, error);

			ok &= expect(write_secure_config(), "restore edit identity source config");
			{
				bool                                                replacement_ok = false;
				const howdy::native::config_test_hooks::ScopedHooks hooks([&]() -> void {
					replacement_ok = replace_with_symlink();
				});

				const bool matches =
				    dependencies.file_content_matches(dependencies.context, config_path, original);
				ok &= expect(replacement_ok && matches,
				             "file_content_matches consumes opened config descriptor");
			}

			fs::remove_all(temp_root, error);
			return ok;
		}

	}  // namespace

	auto run_config_cli_integration_tests() -> bool {
		bool ok = true;
		ok &= public_entrypoint_preserves_invalid_edit();
		ok &= production_config_reads_are_bounded();
		ok &= production_edit_reads_are_descriptor_bound();
		return ok;
	}

}  // namespace howdy::test::config_cli
