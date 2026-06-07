#include "config/config_utils.hpp"
#include "config/config_validation.hpp"

#include <cerrno>
#include <clocale>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

#include <sys/stat.h>

namespace {

    auto write_file(const std::filesystem::path &path, const std::string &content) -> bool {
        std::ofstream out(path);
        if (!out.is_open()) {
            return false;
        }
        out << content;
        return out.good();
    }

    auto read_file(const std::filesystem::path &path) -> std::string {
        std::ifstream in(path);
        if (!in.is_open()) {
            return {};
        }
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    auto count_staged_configs(const std::filesystem::path &directory) -> std::size_t {
        std::size_t count = 0;
        for (const auto &entry : std::filesystem::directory_iterator(directory)) {
            if (entry.path().filename().string().starts_with(".howdy-config-")) {
                ++count;
            }
        }
        return count;
    }

    auto expect(bool condition, const std::string &message) -> bool {
        if (!condition) {
            std::cerr << "FAIL: " << message << "\n";
            return false;
        }
        return true;
    }

    auto get_env_value(const char *name) -> std::optional<std::string> {
        const char *value = std::getenv(name);
        if (value == nullptr) {
            return std::nullopt;
        }
        return std::string(value);
    }

    void restore_env_value(const char *name, const std::optional<std::string> &value) {
        if (value.has_value()) {
            setenv(name, value->c_str(), 1);
            return;
        }
        unsetenv(name);
    }

}  // namespace

auto main() -> int {
    namespace fs = std::filesystem;

    bool            ok        = true;
    const auto      temp_root = fs::current_path() / "howdy-config-utils-test-work";
    std::error_code ec;
    fs::remove_all(temp_root, ec);
    fs::create_directories(temp_root, ec);
    ok &= expect(!ec, "create temp root");

    const auto config_path = temp_root / "config.ini";
    ok &= expect(write_file(config_path, "[core]\n"
                                         "disabled = false\n"
                                         "\n"
                                         "[video]\n"
                                         "dark_threshold = 60\n"),
                 "write initial config");

    const auto lines = howdy::native::read_config_lines(config_path, false);
    ok &= expect(lines.size() == 5, "read_config_lines returns expected line count");
    ok &= expect(!lines.empty() && lines[0] == "[core]\n", "read_config_lines preserves newlines");

    ok &= expect(howdy::native::update_config_value(config_path, "disabled", "true"),
                 "update_config_value succeeds for existing key");
    const auto after_disabled = read_file(config_path);
    ok &= expect(after_disabled.find("disabled = true\n") != std::string::npos,
                 "disabled key updated");

    ok &= expect(howdy::native::is_safe_ini_scalar_value("true"),
                 "is_safe_ini_scalar_value accepts simple scalar");
    ok &= expect(!howdy::native::is_safe_ini_scalar_value("true\n[video]\ntimeout = 0"),
                 "is_safe_ini_scalar_value rejects newline injection");
    ok &= expect(!howdy::native::is_safe_ini_scalar_value("[video]"),
                 "is_safe_ini_scalar_value rejects section-like values");

    ok &= expect(howdy::native::update_config_value(config_path, "dark_threshold", "42"),
                 "update_config_value succeeds in later section");
    const auto after_threshold = read_file(config_path);
    ok &= expect(after_threshold.find("dark_threshold = 42\n") != std::string::npos,
                 "dark_threshold key updated");

    ok &= expect(!howdy::native::update_config_value(config_path, "missing_key", "x"),
                 "update_config_value fails for missing key");
    ok &= expect(!howdy::native::update_config_value(config_path, "dark_threshold",
                                                     "0\n[core]\ndisabled = true"),
                 "update_config_value rejects newline injection");
    ok &= expect(read_file(config_path) == after_threshold,
                 "rejected injection leaves config unchanged");
    ok &= expect(!howdy::native::update_config_value(config_path, "dark_threshold", "1000"),
                 "update_config_value rejects semantically invalid values");
    ok &= expect(read_file(config_path) == after_threshold,
                 "semantic validation failure leaves config unchanged");
    ok &= expect(!howdy::native::update_config_value(config_path, "device_fps", "-1"),
                 "update_config_value rejects invalid device_fps values");
    ok &= expect(read_file(config_path) == after_threshold,
                 "invalid device_fps update leaves config unchanged");

    auto write_float_config = [&]() -> bool {
        return write_file(config_path, "[video]\n"
                                       "clahe_clip_limit = 2\n"
                                       "\n"
                                       "[face]\n"
                                       "yunet_score_threshold = 0.9\n"
                                       "yunet_nms_threshold = 0.4\n"
                                       "sface_threshold = 0.5\n");
    };

    auto expect_float_write_path = [&](const std::string &label) -> bool {
        bool write_ok = true;
        write_ok &= expect(write_float_config(), label + ": write baseline float config");
        write_ok &=
            expect(howdy::native::update_config_value(config_path, "clahe_clip_limit", "1.25"),
                   label + ": update_config_value accepts clahe_clip_limit dot decimal");
        write_ok &=
            expect(read_file(config_path).find("clahe_clip_limit = 1.25\n") != std::string::npos,
                   label + ": clahe_clip_limit written unchanged");
        write_ok &= expect(
            howdy::native::update_config_value(config_path, "yunet_score_threshold", "0.8845"),
            label + ": update_config_value accepts yunet_score_threshold dot decimal");
        write_ok &= expect(read_file(config_path).find("yunet_score_threshold = 0.8845\n") !=
                               std::string::npos,
                           label + ": yunet_score_threshold written unchanged");
        write_ok &=
            expect(howdy::native::update_config_value(config_path, "yunet_nms_threshold", "0.3"),
                   label + ": update_config_value accepts yunet_nms_threshold dot decimal");
        write_ok &=
            expect(read_file(config_path).find("yunet_nms_threshold = 0.3\n") != std::string::npos,
                   label + ": yunet_nms_threshold written unchanged");
        write_ok &=
            expect(howdy::native::update_config_value(config_path, "sface_threshold", "0.6942"),
                   label + ": update_config_value accepts sface_threshold dot decimal");
        write_ok &=
            expect(read_file(config_path).find("sface_threshold = 0.6942\n") != std::string::npos,
                   label + ": sface_threshold written unchanged");

        for (const auto *const value : {"1,25", "1.25abc", "nan", "inf", "+inf", "-inf"}) {
            const auto before_invalid = read_file(config_path);
            write_ok &=
                expect(!howdy::native::update_config_value(config_path, "clahe_clip_limit", value),
                       label + ": update_config_value rejects invalid float " + std::string(value));
            write_ok &= expect(read_file(config_path) == before_invalid,
                               label + ": invalid float update leaves config unchanged for " +
                                   std::string(value));
        }
        return write_ok;
    };

    ok &= expect_float_write_path("C locale");

    const char *current_locale = std::setlocale(LC_ALL, nullptr);
    const auto  previous_locale =
        current_locale == nullptr ? std::optional<std::string>() : std::string(current_locale);
    const auto previous_lc_all     = get_env_value("LC_ALL");
    const auto previous_lc_numeric = get_env_value("LC_NUMERIC");
    const auto previous_lang       = get_env_value("LANG");
    unsetenv("LC_ALL");
    setenv("LANG", "C", 1);
    setenv("LC_NUMERIC", "nl_NL.UTF-8", 1);
    if (std::setlocale(LC_ALL, "") != nullptr) {
        ok &= expect_float_write_path("LC_NUMERIC=nl_NL.UTF-8");
    } else {
        std::cerr << "SKIP: nl_NL.UTF-8 locale is not generated\n";
    }
    restore_env_value("LC_ALL", previous_lc_all);
    restore_env_value("LC_NUMERIC", previous_lc_numeric);
    restore_env_value("LANG", previous_lang);
    if (previous_locale.has_value()) {
        std::setlocale(LC_ALL, previous_locale->c_str());
    }

    ok &= expect(write_file(config_path, "[core]\n"
                                         "disabled = false\n"
                                         "\n"
                                         "[video]\n"
                                         "timeout = 0\n"),
                 "write config with invalid timeout");
    ok &= expect(
        howdy::native::update_config_value(config_path, "disabled", "true", nullptr, false, false),
        "update_config_value can bypass runtime validation for recovery writes");
    const auto after_disable_recovery = read_file(config_path);
    ok &= expect(after_disable_recovery.find("disabled = true\n") != std::string::npos,
                 "recovery write updates disabled key");
    howdy::native::ConfigReader still_invalid(config_path.string());
    ok &= expect(still_invalid.ok(), "recovery-write config still parses");
    ok &= expect(howdy::native::validate_runtime_config(still_invalid).has_value(),
                 "recovery write does not mask unrelated invalid config values");
    ok &= expect(write_file(config_path, after_threshold),
                 "restore valid config after recovery-write test");

    const auto                     nested_path = temp_root / "nested" / "generated.ini";
    const std::vector<std::string> write_lines = {
        "[face]\n",
        "sface_threshold = 0.363\n",
    };
    ok &= expect(howdy::native::atomic_write_lines(nested_path, write_lines),
                 "atomic_write_lines creates parent dirs and writes file");
    ok &= expect(read_file(nested_path) == "[face]\nsface_threshold = 0.363\n",
                 "atomic_write_lines output matches expected content");

    const std::string valid_content = "[core]\n"
                                      "disabled = false\n"
                                      "\n"
                                      "[video]\n"
                                      "dark_threshold = 50\n";
    std::string       validation_error;
    ok &= expect(howdy::native::validate_config_content(valid_content, &validation_error),
                 "validate_config_content accepts valid config");
    validation_error.clear();
    ok &= expect(!howdy::native::validate_config_content("[core\n", &validation_error),
                 "validate_config_content rejects invalid syntax");
    ok &= expect(validation_error == "Updated config is invalid",
                 "invalid config syntax reports stable invalid-config error");
    validation_error.clear();
    ok &=
        expect(!howdy::native::validate_config_content("[video]\ntimeout = 0\n", &validation_error),
               "validate_config_content rejects invalid runtime semantics");
    ok &= expect(!validation_error.empty(), "invalid runtime config reports an error message");
    ok &= expect(validation_error != "Updated config is invalid",
                 "invalid runtime config reports runtime validation error");

    const auto replace_path = temp_root / "replace.ini";
    ok &= expect(write_file(replace_path, valid_content), "write replace config baseline");
    ok &= expect(chmod(replace_path.c_str(), 0600) == 0, "set replace config mode");
    const std::string replacement_content = "[core]\n"
                                            "disabled = true\n"
                                            "\n"
                                            "[video]\n"
                                            "dark_threshold = 55\n";
    std::string       install_error;
    ok &= expect(howdy::native::replace_config_content_atomically(replace_path, replacement_content,
                                                                  &install_error, true, true),
                 "replace_config_content_atomically installs valid content with lock");
    ok &= expect(read_file(replace_path) == replacement_content,
                 "replace_config_content_atomically writes expected content");
    struct stat replace_stat{};
    ok &= expect(stat(replace_path.c_str(), &replace_stat) == 0, "stat replaced config");
    ok &= expect((replace_stat.st_mode & 0777) == 0600,
                 "replace_config_content_atomically preserves config mode");

    const auto before_invalid_replace = read_file(replace_path);
    const auto staged_before          = count_staged_configs(temp_root);
    ok &= expect(!howdy::native::replace_config_content_atomically(
                     replace_path, "[video]\ntimeout = 0\n", &install_error, true, true),
                 "replace_config_content_atomically rejects invalid content with lock");
    ok &= expect(read_file(replace_path) == before_invalid_replace,
                 "invalid replacement leaves old config unchanged");
    ok &= expect(count_staged_configs(temp_root) == staged_before,
                 "invalid replacement leaves no staged config");

    const std::string recovery_content = "[core]\n"
                                         "disabled = true\n"
                                         "\n"
                                         "[video]\n"
                                         "timeout = 0\n";
    ok &= expect(howdy::native::replace_config_content_atomically(replace_path, recovery_content,
                                                                  &install_error, true, false),
                 "replace_config_content_atomically releases lock and can bypass validation");
    ok &= expect(read_file(replace_path) == recovery_content,
                 "runtime-validation bypass installs content");

    const auto stale_expected_content = recovery_content;
    ok &= expect(write_file(replace_path, valid_content), "write changed config before stale edit");
    ok &= expect(chmod(replace_path.c_str(), 0600) == 0, "restore changed config mode");
    ok &= expect(!howdy::native::replace_config_content_atomically(
                     replace_path, replacement_content, &install_error, true, false,
                     &stale_expected_content),
                 "replace_config_content_atomically rejects stale expected content");
    ok &= expect(read_file(replace_path) == valid_content,
                 "stale expected content leaves current config unchanged");

    ok &= expect(chmod(replace_path.c_str(), 0666) == 0, "make replace config world-writable");
    ok &= expect(!howdy::native::replace_config_content_atomically(replace_path, valid_content,
                                                                   &install_error, true, true),
                 "replace_config_content_atomically rejects insecure config permissions with lock");
    ok &= expect(chmod(replace_path.c_str(), 0600) == 0, "restore replace config permissions");

    const auto replace_insecure_dir = temp_root / "replace-insecure-dir";
    ok &= expect(fs::create_directories(replace_insecure_dir, ec) || !ec,
                 "create insecure replace dir");
    ok &= expect(!ec, "no error creating insecure replace dir");
    const auto replace_insecure_path = replace_insecure_dir / "config.ini";
    ok &= expect(write_file(replace_insecure_path, valid_content), "write insecure replace config");
    ok &= expect(chmod(replace_insecure_dir.c_str(), 0777) == 0,
                 "make replace config dir world-writable");
    ok &= expect(!howdy::native::replace_config_content_atomically(
                     replace_insecure_path, replacement_content, &install_error, false, true),
                 "replace_config_content_atomically rejects insecure parent directory");
    ok &= expect(chmod(replace_insecure_dir.c_str(), 0755) == 0, "restore replace config dir mode");

    const auto non_regular_path = temp_root / "non-regular.ini";
    ok &= expect(fs::create_directory(non_regular_path, ec), "create non-regular config target");
    ok &= expect(!howdy::native::replace_config_content_atomically(non_regular_path, valid_content,
                                                                   &install_error, false, true),
                 "replace_config_content_atomically rejects non-regular config target");

    ok &= expect(chmod(config_path.c_str(), 0666) == 0, "make config file world-writable");
    ok &= expect(!howdy::native::update_config_value(config_path, "disabled", "false"),
                 "update_config_value rejects insecure config permissions");
    ok &= expect(chmod(config_path.c_str(), 0644) == 0, "restore config permissions");

    const auto insecure_dir = temp_root / "insecure-dir";
    ok &= expect(fs::create_directories(insecure_dir, ec) || !ec, "create insecure config dir");
    ok &= expect(!ec, "no error creating insecure config dir");
    const auto insecure_config_path = insecure_dir / "config.ini";
    ok &= expect(write_file(insecure_config_path, "[core]\ndisabled = false\n"),
                 "write config in insecure dir");
    ok &= expect(chmod(insecure_dir.c_str(), 0777) == 0, "make config dir world-writable");
    ok &= expect(!howdy::native::check_secure_config_path(insecure_config_path).ok,
                 "check_secure_config_path rejects insecure config directory");
    ok &= expect(!howdy::native::update_config_value(insecure_config_path, "disabled", "true"),
                 "update_config_value rejects insecure config directory");
    ok &= expect(chmod(insecure_dir.c_str(), 0755) == 0, "restore config dir mode");

    const auto insecure_ancestor_root = temp_root / "insecure-ancestor";
    const auto nested_config_dir      = insecure_ancestor_root / "nested" / "deeper";
    ok &= expect(fs::create_directories(nested_config_dir, ec) || !ec, "create nested config dir");
    ok &= expect(!ec, "no error creating nested config dir");
    const auto nested_config_path = nested_config_dir / "config.ini";
    ok &= expect(write_file(nested_config_path, "[core]\ndisabled = false\n"),
                 "write config in nested dir");
    ok &= expect(chmod(insecure_ancestor_root.c_str(), 0777) == 0,
                 "make ancestor config dir world-writable");
    ok &= expect(!howdy::native::check_secure_config_path(nested_config_path).ok,
                 "check_secure_config_path rejects insecure ancestor directory");
    ok &= expect(!howdy::native::update_config_value(nested_config_path, "disabled", "true"),
                 "update_config_value rejects insecure ancestor directory");
    ok &= expect(chmod(insecure_ancestor_root.c_str(), 0755) == 0,
                 "restore ancestor config dir mode");

    const auto unreadable_dir = temp_root / "unreadable-dir";
    ok &= expect(fs::create_directories(unreadable_dir, ec) || !ec, "create unreadable config dir");
    ok &= expect(!ec, "no error creating unreadable config dir");
    const auto unreadable_config_path = unreadable_dir / "config.ini";
    ok &= expect(write_file(unreadable_config_path, "[core]\ndisabled = false\n"),
                 "write config in unreadable dir");
    ok &= expect(chmod(unreadable_dir.c_str(), 0000) == 0, "make config dir unreadable");
    if (geteuid() != 0) {
        const auto unreadable_check =
            howdy::native::check_secure_config_path(unreadable_config_path);
        ok &= expect(!unreadable_check.ok,
                     "check_secure_config_path rejects inaccessible config path");
        ok &= expect(unreadable_check.error_code == EACCES,
                     "inaccessible config path preserves EACCES");
        ok &= expect(unreadable_check.error_message.find("process uid=") != std::string::npos,
                     "inaccessible config path reports process uid");
        ok &= expect(unreadable_check.error_message.find("do not make /etc/howdy") !=
                         std::string::npos,
                     "inaccessible config path warns against insecure permissions");
    }
    ok &= expect(chmod(unreadable_dir.c_str(), 0755) == 0, "restore unreadable config dir mode");

    const auto protected_path = temp_root / "protected.ini";
    ok &=
        expect(write_file(protected_path, "[core]\ndisabled = false\n"), "write protected config");
    ok &= expect(chmod(protected_path.c_str(), 0600) == 0, "set protected config mode");
    const auto strict_root_check =
        howdy::native::check_secure_config_path(protected_path, static_cast<uid_t>(0));
    if (geteuid() != 0) {
        ok &= expect(!strict_root_check.ok,
                     "strict root-owned config check rejects non-root-owned config");
        ok &= expect(strict_root_check.error_message.find("owned by root") != std::string::npos,
                     "strict root-owned config check reports root ownership requirement");
    } else {
        ok &= expect(strict_root_check.ok,
                     "strict root-owned config check accepts root-owned config");
    }
    ok &= expect(howdy::native::update_config_value(protected_path, "disabled", "true"),
                 "update_config_value succeeds on secure config");
    struct stat protected_stat{};
    ok &= expect(stat(protected_path.c_str(), &protected_stat) == 0, "stat protected config");
    ok &= expect((protected_stat.st_mode & 0777) == 0600, "atomic write preserves config mode");

    const auto hardlink_path = temp_root / "protected-hardlink.ini";
    ok &= expect(link(protected_path.c_str(), hardlink_path.c_str()) == 0,
                 "create hard link to protected config");
    ok &= expect(!howdy::native::update_config_value(hardlink_path, "disabled", "false"),
                 "update_config_value rejects hard-linked config");

    howdy::native::ConfigReader validated(config_path.string());
    ok &= expect(validated.ok(), "validated config still parses");
    ok &= expect(!howdy::native::validate_runtime_config(validated).has_value(),
                 "validated config passes semantic validation");

    fs::remove_all(temp_root, ec);
    if (!ok) {
        return 1;
    }
    return 0;
}
