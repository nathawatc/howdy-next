#include "auth_helper_testing.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

#include <sys/stat.h>

namespace {

    class ScopedFd {
    public:
        ScopedFd() = default;

        explicit ScopedFd(int fd)
            : fd_(fd) {}

        ScopedFd(const ScopedFd &)                     = delete;
        auto operator=(const ScopedFd &) -> ScopedFd & = delete;

        ScopedFd(ScopedFd &&other) noexcept
            : fd_(other.release()) {}

        auto operator=(ScopedFd &&other) noexcept -> ScopedFd & {
            if (this != &other) {
                reset(other.release());
            }
            return *this;
        }

        ~ScopedFd() {
            reset();
        }

        [[nodiscard]] auto get() const -> int {
            return fd_;
        }

        void reset(int fd = -1) {
            if (fd_ >= 0) {
                close(fd_);
            }
            fd_ = fd;
        }

        auto release() -> int {
            const int fd = fd_;
            fd_          = -1;
            return fd;
        }

    private:
        int fd_ = -1;
    };

    auto expect(bool condition, const std::string &message) -> bool {
        if (!condition) {
            std::cerr << "FAIL: " << message << "\n";
            return false;
        }
        return true;
    }

    auto write_file(const std::filesystem::path &path, const std::string &content) -> bool {
        std::ofstream output(path);
        output << content;
        return output.good();
    }

    auto read_file(const std::filesystem::path &path) -> std::string {
        std::ifstream input(path);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    auto expect_runtime_root_validation(const std::filesystem::path &temp_root) -> bool {
        using howdy::native::testing::runtime_root;
        using howdy::native::testing::validate_runtime_root;

        bool ok = true;
        ok &= expect(runtime_root() == "/run/howdy", "runtime root is fixed under /run/howdy");

        const auto regular_path = temp_root / "runtime-root-file";
        ok &= expect(write_file(regular_path, "not a directory"), "writes runtime root file");
        ok &= expect(!validate_runtime_root(regular_path), "regular runtime root path is rejected");

        const auto      user_owned_dir = temp_root / "runtime-root-dir";
        std::error_code ec;
        std::filesystem::create_directory(user_owned_dir, ec);
        ok &= expect(!ec, "creates runtime root directory fixture");
        if (geteuid() == 0) {
            ok &= expect(validate_runtime_root(user_owned_dir),
                         "root-owned runtime root directory is accepted");
        } else {
            ok &= expect(!validate_runtime_root(user_owned_dir),
                         "user-owned runtime root directory is rejected");
        }

        return ok;
    }

    auto expect_secure_source_file_stat(const std::filesystem::path &temp_root) -> bool {
        using howdy::native::testing::secure_source_file_stat;

        bool ok = true;
        ok &= expect(!secure_source_file_stat(-1, "Invalid fd"), "invalid fd is rejected");

        ScopedFd dir_fd(open(temp_root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        ok &= expect(dir_fd.get() >= 0, "opens directory fd");
        ok &= expect(!secure_source_file_stat(dir_fd.get(), "Directory"),
                     "directory fd is rejected as source file");

        const auto regular_path = temp_root / "source-file";
        ok &= expect(write_file(regular_path, "source"), "writes source file");
        ok &= expect(chmod(regular_path.c_str(), 0664) == 0, "makes source group-writable");
        ScopedFd group_writable_fd(open(regular_path.c_str(), O_RDONLY | O_CLOEXEC));
        ok &= expect(group_writable_fd.get() >= 0, "opens group-writable source");
        ok &= expect(!secure_source_file_stat(group_writable_fd.get(), "Group writable source"),
                     "group-writable source is rejected");
        group_writable_fd.reset();

        ok &= expect(chmod(regular_path.c_str(), 0644) == 0, "restores source mode");
        ScopedFd regular_fd(open(regular_path.c_str(), O_RDONLY | O_CLOEXEC));
        ok &= expect(regular_fd.get() >= 0, "opens regular source");
        if (geteuid() == 0) {
            ok &= expect(secure_source_file_stat(regular_fd.get(), "Root source"),
                         "root-owned regular source is accepted");
        } else {
            ok &= expect(!secure_source_file_stat(regular_fd.get(), "User source"),
                         "non-root-owned regular source is rejected");
        }

        return ok;
    }

    auto expect_write_all_helper(const std::filesystem::path &temp_root) -> bool {
        using howdy::native::testing::write_all;

        bool ok = true;
        ok &= expect(!write_all(-1, "x", 1), "invalid write fd is rejected");

        const auto output_path = temp_root / "write-all-output";
        ScopedFd   output_fd(
            open(output_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
        ok &= expect(output_fd.get() >= 0, "creates write-all output file");
        constexpr auto kOutput = "alpha\nbeta\n";
        ok &=
            expect(write_all(output_fd.get(), kOutput, static_cast<ssize_t>(std::strlen(kOutput))),
                   "writes complete buffer");
        output_fd.reset();
        ok &= expect(read_file(output_path) == kOutput, "write-all output matches input");

        return ok;
    }

    auto expect_copy_file_for_user(const std::filesystem::path &temp_root) -> bool {
        using howdy::native::testing::copy_file_for_user;

        bool ok = true;
        ok &= expect(
            !copy_file_for_user(temp_root / "missing", temp_root / "dest", "Missing", getgid()),
            "missing source copy fails");

        const auto real_source    = temp_root / "real-source";
        const auto symlink_source = temp_root / "symlink-source";
        ok &= expect(write_file(real_source, "real"), "writes real source");
        if (symlink(real_source.c_str(), symlink_source.c_str()) == 0) {
            ok &= expect(!copy_file_for_user(symlink_source, temp_root / "symlink-dest", "Symlink",
                                             getgid()),
                         "symlink source copy fails closed");
        } else {
            std::cerr << "SKIP: symlink source creation failed: " << std::strerror(errno) << "\n";
        }

        if (geteuid() == 0) {
            const auto destination = temp_root / "copied-source";
            ok &= expect(chmod(real_source.c_str(), 0644) == 0, "sets secure source mode");
            ok &= expect(copy_file_for_user(real_source, destination, "Source", getgid()),
                         "secure source copy succeeds as root");
            ok &= expect(read_file(destination) == "real", "copied file preserves content");

            struct stat stat_{};
            ok &= expect(lstat(destination.c_str(), &stat_) == 0, "stats copied file");
            ok &= expect(stat_.st_uid == 0 && stat_.st_gid == getgid(),
                         "copied file owner is root and invoking group");
            ok &= expect((stat_.st_mode & 0777) == 0440, "copied file mode is restricted");
            ok &= expect(!copy_file_for_user(real_source, destination, "Existing", getgid()),
                         "existing destination copy fails");
        } else {
            std::cerr << "SKIP: successful auth-helper copy requires root-owned source\n";
        }

        return ok;
    }

    auto expect_prepare_cleanup_guards() -> bool {
        using howdy::native::testing::cleanup_for_user;
        using howdy::native::testing::prepare_for_user;

        bool ok = true;
        if (geteuid() == 0) {
            ok &= expect(prepare_for_user("../alice") == 1, "root prepare rejects invalid user");
            ok &= expect(cleanup_for_user("/tmp/not-howdy-runtime") == 1,
                         "root cleanup rejects unexpected path");
            const auto missing_expected = std::filesystem::path("/run/howdy") /
                                          ("pam-" + std::to_string(getuid()) + "-missing");
            ok &= expect(cleanup_for_user(missing_expected) == 0,
                         "root cleanup accepts missing expected runtime dir");
        } else {
            ok &= expect(prepare_for_user("../alice") == 1, "non-root prepare fails closed");
            ok &= expect(cleanup_for_user("/run/howdy/pam-0-missing") == 1,
                         "non-root cleanup fails closed");
        }

        return ok;
    }

}  // namespace

auto main() -> int {
    namespace fs = std::filesystem;

    bool            ok        = true;
    const auto      temp_root = fs::temp_directory_path() / "howdy-auth-helper-test";
    std::error_code ec;
    fs::remove_all(temp_root, ec);
    fs::create_directories(temp_root, ec);
    ok &= expect(!ec, "creates auth-helper temp root");

    ok &= expect_runtime_root_validation(temp_root);
    ok &= expect_secure_source_file_stat(temp_root);
    ok &= expect_write_all_helper(temp_root);
    ok &= expect_copy_file_for_user(temp_root);
    ok &= expect_prepare_cleanup_guards();

    fs::remove_all(temp_root, ec);
    return ok ? 0 : 1;
}
