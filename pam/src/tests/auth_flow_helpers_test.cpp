#include "auth_flow_testing.hpp"

#include <array>
#include <cerrno>
#include <iostream>
#include <string>
#include <unistd.h>

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

    auto open_pipe(std::array<ScopedFd, 2> *fds) -> bool {
        std::array<int, 2> raw_fds{{-1, -1}};
        if (pipe(raw_fds.data()) != 0) {
            return false;
        }
        (*fds)[0].reset(raw_fds[0]);
        (*fds)[1].reset(raw_fds[1]);
        return true;
    }

    auto write_all(int fd, const std::string &data) -> bool {
        std::size_t offset = 0;
        while (offset < data.size()) {
            const ssize_t result = write(fd, data.data() + offset, data.size() - offset);
            if (result < 0 && errno == EINTR) {
                continue;
            }
            if (result <= 0) {
                return false;
            }
            offset += static_cast<std::size_t>(result);
        }
        return true;
    }

    auto expect_fd_reading() -> bool {
        using howdy::pam::testing::read_fd_to_string;

        bool                    ok = true;
        std::array<ScopedFd, 2> empty_pipe;
        ok &= expect(open_pipe(&empty_pipe), "creates empty input pipe");
        empty_pipe[1].reset();
        ok &= expect(read_fd_to_string(empty_pipe[0].get()).empty(), "reads empty input");
        ok &= expect(read_fd_to_string(-1).empty(), "read failure returns collected empty output");

        std::array<ScopedFd, 2> small_pipe;
        ok &= expect(open_pipe(&small_pipe), "creates small input pipe");
        ok &= expect(write_all(small_pipe[1].get(), "CONFIG_PATH=/run/howdy/config.ini\n"),
                     "writes small helper output");
        small_pipe[1].reset();
        ok &=
            expect(read_fd_to_string(small_pipe[0].get()) == "CONFIG_PATH=/run/howdy/config.ini\n",
                   "reads complete small helper output");

        std::string temp_path = "/tmp/howdy-auth-flow-output-XXXXXX";
        ScopedFd    bounded_fd(mkstemp(temp_path.data()));
        unlink(temp_path.c_str());
        ok &= expect(bounded_fd.get() >= 0, "creates bounded input file");
        const std::string oversized_output(16384, 'x');
        ok &=
            expect(write_all(bounded_fd.get(), oversized_output), "writes oversized helper output");
        ok &= expect(lseek(bounded_fd.get(), 0, SEEK_SET) == 0, "rewinds oversized helper output");
        const std::string bounded_output = read_fd_to_string(bounded_fd.get());
        ok &= expect(bounded_output == oversized_output.substr(0, 9216),
                     "stops reading after bounded output threshold");

        return ok;
    }

}  // namespace

auto main() -> int {
    using howdy::pam::testing::helper_output_value;

    bool ok = true;

    ok &= expect_fd_reading();

    const std::string output =
        "NOTICE=ignored\nCONFIG_PATH=/run/howdy/config.ini\nUSER_MODELS_DIR=/run/howdy/models\n";
    ok &= expect(helper_output_value(output, "CONFIG_PATH") == "/run/howdy/config.ini",
                 "extracts config path");
    ok &= expect(helper_output_value(output, "USER_MODELS_DIR") == "/run/howdy/models",
                 "extracts user models directory");
    ok &= expect(helper_output_value("CONFIG_PATH=/run/howdy=config.ini\n", "CONFIG_PATH") ==
                     "/run/howdy=config.ini",
                 "preserves equals characters in value");
    ok &= expect(helper_output_value("CONFIG_PATH_EXTRA=wrong\nCONFIG_PATH=right", "CONFIG_PATH") ==
                     "right",
                 "matches exact key and parses final line");
    ok &= expect(helper_output_value(output, "MISSING").empty(), "missing key returns empty value");
    ok &= expect(helper_output_value("CONFIG_PATH=\n", "CONFIG_PATH").empty(),
                 "empty helper value remains empty");

    return ok ? 0 : 1;
}
