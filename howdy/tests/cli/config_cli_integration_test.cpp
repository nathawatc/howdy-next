#include "cli/config/edit_session.hpp"
#include "cli/config/internal.hpp"
#include "cli/config_cli_test_support.hpp"
#include "config/config_limits.hpp"
#include "config/test_hooks.hpp"
#include "test_support.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <pwd.h>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

#include <sys/stat.h>

namespace howdy::test::config_cli {

	using howdy::test::Expect;
	using howdy::test::ReadFile;
	using howdy::test::WriteFile;

	namespace {

		constexpr std::string_view kIntegrationOriginalContent = "[core]\ndisabled = false\n";

		class ScopedEnvironmentVariable {
		public:
			ScopedEnvironmentVariable(const char *name, const std::string &value)
			    : name_(name) {
				SaveOriginal();
				setenv(name_, value.c_str(), 1);
			}

			explicit ScopedEnvironmentVariable(const char *name)
			    : name_(name) {
				SaveOriginal();
				unsetenv(name_);
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

			void Set(const std::string &value) {
				setenv(name_, value.c_str(), 1);
			}

			void Unset() {
				unsetenv(name_);
			}

		private:
			void SaveOriginal() {
				if (const char *current = std::getenv(name_); current != nullptr) {
					original_ = current;
				}
			}

			const char                *name_;
			std::optional<std::string> original_;
		};

		struct EditorPasswdFixture {
			uid_t       uid;
			gid_t       gid;
			std::string name;
		};

		auto CopyEditorPasswdFixture(const passwd *pwd) -> std::optional<EditorPasswdFixture> {
			if (pwd == nullptr || pwd->pw_name == nullptr || pwd->pw_name[0] == '\0') {
				return std::nullopt;
			}
			return EditorPasswdFixture{
			    .uid = pwd->pw_uid, .gid = pwd->pw_gid, .name = pwd->pw_name};
		}

		auto FindEditorPasswdFixture() -> std::optional<EditorPasswdFixture> {
			if (const auto current = CopyEditorPasswdFixture(getpwuid(getuid()));
			    current.has_value() && current->uid != 0) {
				return current;
			}

			std::optional<EditorPasswdFixture> fixture;
			setpwent();
			while (const passwd *pwd = getpwent()) {
				if (pwd->pw_uid == 0) {
					continue;
				}
				fixture = CopyEditorPasswdFixture(pwd);
				if (fixture.has_value()) {
					break;
				}
			}
			endpwent();
			return fixture;
		}

		auto DirectRootHonorsEnvironmentEditor() -> bool {
			ScopedEnvironmentVariable editor_env("EDITOR", "nvim");
			ScopedEnvironmentVariable sudo_user_env("SUDO_USER");
			ScopedEnvironmentVariable sudo_uid_env("SUDO_UID");
			ScopedEnvironmentVariable sudo_gid_env("SUDO_GID");
			ScopedEnvironmentVariable doas_user_env("DOAS_USER");
			ScopedEnvironmentVariable pkexec_uid_env("PKEXEC_UID");

			const auto dependencies =
			    howdy::native::config_internal::DefaultConfigEditDependencies();
			const auto identity = dependencies.resolve_invoking_identity(dependencies.context);
			const auto editor   = dependencies.select_editor_preference(dependencies.context);
			return Expect(identity.status ==
			                      howdy::native::InvokingIdentityStatus::kNoWrapperIdentity &&
			                  !identity.user.has_value() && editor == "nvim",
			              "direct-root config editing honors environment editor");
		}

		auto EditorPreferenceSelection() -> bool {
			const auto dependencies =
			    howdy::native::config_internal::DefaultConfigEditDependencies();
			bool ok = true;

			{
				ScopedEnvironmentVariable editor_env("EDITOR", "nvim");
				ok &= Expect(dependencies.select_editor_preference(dependencies.context) == "nvim",
				             "bare EDITOR is accepted");
			}
			{
				ScopedEnvironmentVariable editor_env("EDITOR", "/some/test/path/nvim");
				ok &= Expect(dependencies.select_editor_preference(dependencies.context) ==
				                 "/some/test/path/nvim",
				             "absolute EDITOR path is accepted");
			}
			{
				ScopedEnvironmentVariable editor_env("EDITOR", "nvim -f");
				ok &= Expect(dependencies.select_editor_preference(dependencies.context) == "micro",
				             "command-line EDITOR is rejected");
			}

			return ok;
		}

		auto FallbackEditorExecution() -> bool {
			namespace fs = std::filesystem;

			const auto editor_fixture = FindEditorPasswdFixture();
			if (!editor_fixture.has_value()) {
				return Expect(false, "fallback integration finds non-root editor fixture");
			}
			const auto &editor_identity = *editor_fixture;

			const std::string temp_template =
			    (fs::current_path() / "howdy-config-editor-fallback-XXXXXX").string();
			std::vector<char> writable_template(temp_template.begin(), temp_template.end());
			writable_template.push_back('\0');
			const char *created_path = mkdtemp(writable_template.data());
			if (!Expect(created_path != nullptr, "fallback integration creates temp directory")) {
				return false;
			}

			bool              ok             = true;
			const fs::path    temp_root      = created_path;
			const fs::path    config_path    = temp_root / "config.ini";
			const fs::path    editor_bin     = temp_root / "bin";
			const fs::path    micro_path     = editor_bin / "micro";
			const fs::path    nano_path      = editor_bin / "nano";
			const fs::path    vi_path        = editor_bin / "vi";
			const fs::path    nvim_path      = editor_bin / "nvim";
			const fs::path    absolute_path  = temp_root / "editor";
			const fs::path    malformed_path = editor_bin / "malformed-editor";
			const fs::path    selected_path  = temp_root / "selected-editor";
			const fs::path    shell_marker   = temp_root / "shell-executed";
			const std::string original       = "[core]\ndisabled = false\n";
			std::error_code   error;

			if (editor_identity.uid != getuid()) {
				ok &= Expect(chmod(temp_root.c_str(), 0755) == 0,
				             "fallback integration exposes temp directory");
			}
			ok &= Expect(fs::create_directory(editor_bin, error) && !error,
			             "fallback integration creates editor PATH directory");
			ok &= Expect(chmod(editor_bin.c_str(), 0755) == 0,
			             "fallback integration exposes editor PATH directory");
			ok &= Expect(WriteFile(config_path, original) && chmod(config_path.c_str(), 0600) == 0,
			             "fallback integration writes secure config");
			ok &= Expect(WriteFile(selected_path, {}) && chmod(selected_path.c_str(), 0600) == 0,
			             "fallback integration creates selection log");
			if (editor_identity.uid != getuid()) {
				ok &= Expect(
				    chown(selected_path.c_str(), editor_identity.uid, editor_identity.gid) == 0,
				    "fallback integration assigns selection log");
			}

			auto write_editor = [&](const fs::path &path, std::string_view name) -> bool {
				const std::string script = "#!/bin/sh\nprintf '" + std::string(name) +
				                           "\\n' > \"$HOWDY_TEST_SELECTED_EDITOR\"\nprintf "
				                           "'[core]\\ndisabled = true\\n' > \"$1\"\n";
				return WriteFile(path, script) && chmod(path.c_str(), 0755) == 0;
			};
			ok &= Expect(write_editor(micro_path, "micro"), "fallback integration writes micro");
			ok &= Expect(write_editor(nano_path, "nano"), "fallback integration writes nano");
			ok &= Expect(write_editor(vi_path, "vi"), "fallback integration writes vi");
			ok &= Expect(write_editor(nvim_path, "nvim"), "direct-root integration writes nvim");
			ok &= Expect(write_editor(absolute_path, "absolute"),
			             "direct-root integration writes absolute editor");
			ok &= Expect(WriteFile(malformed_path, ": > \"$HOWDY_TEST_SHELL_MARKER\"\n") &&
			                 chmod(malformed_path.c_str(), 0755) == 0,
			             "fallback integration writes malformed editor");

			const std::string         path_value = editor_bin.string();
			ScopedEnvironmentVariable editor_env("EDITOR");
			ScopedEnvironmentVariable path_env("PATH", path_value);
			ScopedEnvironmentVariable config_env("HOWDY_CONFIG", config_path.string());
			ScopedEnvironmentVariable selected_env("HOWDY_TEST_SELECTED_EDITOR",
			                                       selected_path.string());
			ScopedEnvironmentVariable marker_env("HOWDY_TEST_SHELL_MARKER", shell_marker.string());
			const std::string         invoking_name = editor_identity.name;
			ScopedEnvironmentVariable sudo_user_env("SUDO_USER", invoking_name);
			ScopedEnvironmentVariable sudo_uid_env("SUDO_UID", std::to_string(editor_identity.uid));
			ScopedEnvironmentVariable sudo_gid_env("SUDO_GID", std::to_string(editor_identity.gid));
			ScopedEnvironmentVariable doas_user_env("DOAS_USER");
			ScopedEnvironmentVariable pkexec_uid_env("PKEXEC_UID");

			auto run_config = [&]() -> std::pair<int, std::string> {
				if (!WriteFile(config_path, original) || chmod(config_path.c_str(), 0600) != 0 ||
				    !WriteFile(selected_path, {})) {
					return {1, {}};
				}

				std::array<char *, 1> argv{const_cast<char *>("howdy-config")};
				std::ostringstream    output;
				int                   exit_code = 0;
				{
					ScopedStreamBuffer stdout_guard(std::cout, output.rdbuf());
					howdy::native::file_security_internal::ValidationRoot validation_root{
					    temp_root};
					exit_code = howdy::native::config_internal::ConfigMainWithDependencies(
					    1, argv.data(),
					    howdy::native::config_internal::DefaultConfigEditDependencies(
					        &validation_root));
				}
				return {exit_code, output.str()};
			};

			auto expect_editor = [&](const std::string &preference, std::string_view expected,
			                         std::string_view message) -> void {
				if (preference.empty()) {
					editor_env.Unset();
				} else {
					editor_env.Set(preference);
				}
				const auto result = run_config();
				ok &= Expect(result.first == 0, message);
				ok &= Expect(result.second == "Editing config.ini\nConfig updated\n", message);
				ok &= Expect(ReadFile(selected_path) == expected, message);
			};

			error.clear();
			fs::remove(micro_path, error);
			expect_editor({}, "nano\n", "fallback selects nano when micro is unavailable");
			expect_editor("does-not-exist", "nano\n", "unavailable EDITOR falls back to nano");
			expect_editor(nano_path.string(), "nano\n", "absolute EDITOR executes directly");

			error.clear();
			fs::remove(nano_path, error);
			expect_editor({}, "vi\n", "fallback selects vi after micro and nano");

			ok &= Expect(write_editor(micro_path, "micro"), "fallback integration restores micro");
			ok &= Expect(write_editor(nano_path, "nano"), "fallback integration restores nano");
			expect_editor({}, "micro\n", "fallback order prefers micro");

			error.clear();
			fs::remove(shell_marker, error);
			expect_editor("sh -c touch " + shell_marker.string(), "micro\n",
			              "command-line EDITOR falls back without shell execution");
			ok &= Expect(!fs::exists(shell_marker, error) && !error,
			             "command-line EDITOR cannot execute shell syntax");
			expect_editor("malformed-editor", "micro\n",
			              "malformed EDITOR falls back without implicit shell execution");
			ok &= Expect(!fs::exists(shell_marker, error) && !error,
			             "malformed EDITOR cannot trigger shell execution");

			editor_env.Unset();
			for (const auto &editor_path : {micro_path, nano_path, vi_path}) {
				error.clear();
				ok &= Expect(fs::remove(editor_path, error) && !error,
				             "no-editor fixture removes fallback candidate");
			}
			const auto unavailable_result = run_config();
			ok &= Expect(unavailable_result.first == 1 &&
			                 unavailable_result.second.contains("No suitable text editor found") &&
			                 !unavailable_result.second.contains("Editor exited unsuccessfully"),
			             "no usable editor reports editor unavailable");

			ok &= Expect(WriteFile(micro_path, "#!/bin/sh\nexit 127\n") &&
			                 chmod(micro_path.c_str(), 0755) == 0,
			             "exit-127 fixture writes executable editor");
			const auto exit_127_result = run_config();
			ok &= Expect(
			    exit_127_result.first == 1 &&
			        exit_127_result.second ==
			            "Editing config.ini\nEditor exited unsuccessfully; config not updated\n",
			    "real editor exit 127 reports editor failure");

			sudo_user_env.Unset();
			sudo_uid_env.Unset();
			sudo_gid_env.Unset();
			doas_user_env.Unset();
			pkexec_uid_env.Unset();
			auto expect_direct_root_editor = [&](const std::string &preference,
			                                     std::string_view   expected,
			                                     std::string_view   message) -> void {
				editor_env.Set(preference);
				const auto result = run_config();
				ok &= Expect(result.first == 0 &&
				                 result.second == "Editing config.ini\nConfig updated\n" &&
				                 ReadFile(selected_path) == expected,
				             message);
			};
			expect_direct_root_editor("nvim", "nvim\n", "direct-root bare EDITOR executes fixture");
			expect_direct_root_editor(absolute_path.string(), "absolute\n",
			                          "direct-root absolute EDITOR executes fixture");

			fs::remove_all(temp_root, error);
			return ok;
		}

		auto PathReportedAfter(const std::string &output, const std::string &prefix)
		    -> std::filesystem::path {
			const auto start = output.find(prefix);
			if (start == std::string::npos) {
				return {};
			}
			const auto path_start = start + prefix.size();
			const auto path_end   = output.find('\n', path_start);
			return output.substr(path_start, path_end - path_start);
		}

		auto BoundaryAwareEntrypointPreservesInvalidEdit() -> bool {
			namespace fs              = std::filesystem;
			const auto editor_fixture = FindEditorPasswdFixture();
			if (!editor_fixture.has_value()) {
				return Expect(false, "integration finds non-root editor fixture");
			}
			const auto &editor_identity = *editor_fixture;

			const std::string temp_template =
			    (fs::current_path() / "howdy-config-cli-integration-XXXXXX").string();
			std::vector<char> writable_template(temp_template.begin(), temp_template.end());
			writable_template.push_back('\0');
			const char *created_path = mkdtemp(writable_template.data());
			if (!Expect(created_path != nullptr, "integration creates temp directory")) {
				return false;
			}

			bool            ok              = true;
			const fs::path  temp_root       = created_path;
			const fs::path  config_path     = temp_root / "config.ini";
			const fs::path  editor_bin      = temp_root / "bin";
			const fs::path  editor_path     = editor_bin / "fake-editor";
			const fs::path  credential_path = temp_root / "editor-credentials";
			std::error_code error;
			if (editor_identity.uid != getuid()) {
				ok &= Expect(chmod(temp_root.c_str(), 0755) == 0,
				             "integration exposes editor fixture directory");
			}
			ok &= Expect(fs::create_directory(editor_bin, error) && !error,
			             "integration creates editor PATH directory");
			ok &= Expect(chmod(editor_bin.c_str(), 0755) == 0,
			             "integration exposes editor PATH directory");
			ok &= Expect(WriteFile(config_path, std::string{kIntegrationOriginalContent}),
			             "integration writes config baseline");
			ok &= Expect(chmod(config_path.c_str(), 0600) == 0, "integration secures config file");
			ok &=
			    Expect(WriteFile(editor_path,
			                     "#!/bin/sh\nprintf '%s:%s:%s:%s\\n' \"$(id -u)\" \"$(id -ru)\" "
			                     "\"$(id -g)\" \"$(id -rg)\" > \"$HOWDY_TEST_EDITOR_CREDENTIALS\"\n"
			                     "printf '[core\\n' > \"$1\"\n"),
			           "integration writes fake editor");
			ok &= Expect(chmod(editor_path.c_str(), 0755) == 0,
			             "integration makes fake editor executable");
			ok &= Expect(WriteFile(credential_path, {}), "integration creates credential log");
			ok &= Expect(chmod(credential_path.c_str(), 0600) == 0,
			             "integration secures credential log");
			if (editor_identity.uid != getuid()) {
				ok &= Expect(
				    chown(credential_path.c_str(), editor_identity.uid, editor_identity.gid) == 0,
				    "integration assigns credential log to editor fixture");
			}

			ScopedEnvironmentVariable editor_env("EDITOR", "fake-editor");
			const char               *inherited_path = std::getenv("PATH");
			const std::string         editor_path_value =
			    editor_bin.string() +
			    (inherited_path == nullptr ? "" : ":" + std::string(inherited_path));
			ScopedEnvironmentVariable path_env("PATH", editor_path_value);
			ScopedEnvironmentVariable config_env("HOWDY_CONFIG", config_path.string());
			const std::string         invoking_name = editor_identity.name;
			const auto                invoking_uid  = editor_identity.uid;
			const auto                invoking_gid  = editor_identity.gid;
			ScopedEnvironmentVariable sudo_user_env("SUDO_USER", invoking_name);
			ScopedEnvironmentVariable sudo_uid_env("SUDO_UID", std::to_string(invoking_uid));
			ScopedEnvironmentVariable sudo_gid_env("SUDO_GID", std::to_string(invoking_gid));
			ScopedEnvironmentVariable doas_user_env("DOAS_USER");
			ScopedEnvironmentVariable pkexec_uid_env("PKEXEC_UID");
			ScopedEnvironmentVariable credential_env("HOWDY_TEST_EDITOR_CREDENTIALS",
			                                         credential_path.string());

			std::array<char *, 1> argv{const_cast<char *>("howdy-config")};
			std::ostringstream    output;
			int                   exit_code = 0;
			{
				ScopedStreamBuffer stdout_guard(std::cout, output.rdbuf());
				howdy::native::file_security_internal::ValidationRoot validation_root{temp_root};
				exit_code = howdy::native::config_internal::ConfigMainWithDependencies(
				    1, argv.data(),
				    howdy::native::config_internal::DefaultConfigEditDependencies(
				        &validation_root));
			}

			constexpr std::string_view recovery_prefix =
			    "Edited config is invalid and was not installed: ";
			const auto reported_temp_path =
			    PathReportedAfter(output.str(), std::string(recovery_prefix));
			ok &= Expect(exit_code == 1, "integration aborts invalid edit");
			ok &= Expect(output.str() == "Editing config.ini\n" + std::string(recovery_prefix) +
			                                 reported_temp_path.string() + "\n",
			             "integration output exact");
			ok &= Expect(!reported_temp_path.empty(), "integration reports recovery path");
			ok &= Expect(fs::exists(reported_temp_path, error) && !error,
			             "integration preserves invalid temp file");
			ok &= Expect(ReadFile(reported_temp_path) == "[core\n",
			             "integration keeps malformed content");
			ok &= Expect(ReadFile(credential_path) == std::to_string(invoking_uid) + ":" +
			                                              std::to_string(invoking_uid) + ":" +
			                                              std::to_string(invoking_gid) + ":" +
			                                              std::to_string(invoking_gid) + "\n",
			             "editor runs with validated invoking credentials");
			fs::remove(reported_temp_path, error);
			fs::remove_all(temp_root, error);
			return ok;
		}

		auto ProductionConfigReadsAreBounded() -> bool {
			namespace fs = std::filesystem;

			bool       ok        = true;
			const auto temp_root = fs::current_path() / "howdy-config-cli-size-test";
			fs::create_directory(temp_root);
			ok &= Expect(chmod(temp_root.c_str(), 0700) == 0, "secure size test boundary");
			howdy::native::file_security_internal::ValidationRoot validation_root{temp_root};
			const auto                                            dependencies =
			    howdy::native::config_internal::DefaultConfigEditDependencies(&validation_root);
			const fs::path    source_path = temp_root / "config.ini";
			const std::string at_limit(howdy::native::kMaxConfigFileSize, 'x');
			const std::string over_limit(howdy::native::kMaxConfigFileSize + 1, 'x');
			std::error_code   error;

			ok &= Expect(WriteFile(source_path, at_limit), "write source config at size limit");
			const auto copy =
			    dependencies.create_temp_copy(dependencies.context, source_path, std::nullopt);
			ok &= Expect(copy.has_value() && copy->original_content == at_limit,
			             "source config exactly at size limit is copied");
			if (copy.has_value()) {
				std::string snapshot;
				ok &= Expect(dependencies.read_temp_config_snapshot(dependencies.context,
				                                                    copy->path, &snapshot) &&
				                 snapshot == at_limit,
				             "edited temp config exactly at size limit is read");
				ok &=
				    Expect(WriteFile(copy->path, over_limit), "write oversized edited temp config");
				ok &= Expect(!dependencies.read_temp_config_snapshot(dependencies.context,
				                                                     copy->path, &snapshot),
				             "oversized edited temp config is rejected");
				dependencies.remove_if_exists(dependencies.context, copy->path);
			}

			ok &= Expect(WriteFile(source_path, over_limit), "write oversized source config");
			ok &= Expect(
			    !dependencies.create_temp_copy(dependencies.context, source_path, std::nullopt),
			    "oversized source config is rejected before temp copy");
			fs::remove_all(temp_root, error);
			return ok;
		}

		auto ProductionEditReadsAreDescriptorBound() -> bool {
			namespace fs = std::filesystem;

			const auto temp_template   = fs::current_path() / "howdy-config-cli-identity-XXXXXX";
			const auto template_string = temp_template.string();
			std::vector<char> writable(template_string.begin(), template_string.end());
			writable.push_back('\0');
			const char *created_path = mkdtemp(writable.data());
			if (!Expect(created_path != nullptr, "create edit identity temp directory")) {
				return false;
			}

			bool              ok = true;
			const fs::path    temp_root(created_path);
			const fs::path    config_path = temp_root / "config.ini";
			const fs::path    backup_path = temp_root / "config-backup.ini";
			const std::string original    = "[core]\ndisabled = false\n";
			howdy::native::file_security_internal::ValidationRoot validation_root{temp_root};
			const auto                                            dependencies =
			    howdy::native::config_internal::DefaultConfigEditDependencies(&validation_root);
			std::error_code error;

			auto write_secure_config = [&]() -> bool {
				return WriteFile(config_path, original) && chmod(config_path.c_str(), 0644) == 0;
			};
			auto replace_with_symlink = [&]() -> bool {
				return rename(config_path.c_str(), backup_path.c_str()) == 0 &&
				       symlink("/dev/null", config_path.c_str()) == 0;
			};

			ok &= Expect(write_secure_config(), "write edit identity source config");
			{
				bool                                                replacement_ok = false;
				const howdy::native::config_test_hooks::ScopedHooks hooks([&]() -> void {
					replacement_ok = replace_with_symlink();
				});

				const auto copy =
				    dependencies.create_temp_copy(dependencies.context, config_path, std::nullopt);
				ok &= Expect(replacement_ok, "create_temp_copy hook replaces source pathname");
				ok &= Expect(copy.has_value() && copy->original_content == original,
				             "create_temp_copy consumes opened config descriptor");
				if (copy.has_value()) {
					dependencies.remove_if_exists(dependencies.context, copy->path);
				}
			}
			fs::remove(config_path, error);
			fs::remove(backup_path, error);

			ok &= Expect(write_secure_config(), "restore edit identity source config");
			{
				bool                                                replacement_ok = false;
				const howdy::native::config_test_hooks::ScopedHooks hooks([&]() -> void {
					replacement_ok = replace_with_symlink();
				});

				const bool matches =
				    dependencies.file_content_matches(dependencies.context, config_path, original);
				ok &= Expect(replacement_ok && matches,
				             "file_content_matches consumes opened config descriptor");
			}

			fs::remove_all(temp_root, error);
			return ok;
		}

	}  // namespace

	auto RunConfigCliIntegrationTests() -> bool {
		bool ok = true;
		ok &= DirectRootHonorsEnvironmentEditor();
		ok &= EditorPreferenceSelection();
		ok &= BoundaryAwareEntrypointPreservesInvalidEdit();
		ok &= FallbackEditorExecution();
		ok &= ProductionConfigReadsAreBounded();
		ok &= ProductionEditReadsAreDescriptorBound();
		return ok;
	}

}  // namespace howdy::test::config_cli
