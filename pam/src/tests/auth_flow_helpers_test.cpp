#include "auth_flow_testing.hpp"

#include <iostream>
#include <string>

namespace {

    auto expect(bool condition, const std::string &message) -> bool {
        if (!condition) {
            std::cerr << "FAIL: " << message << "\n";
            return false;
        }
        return true;
    }

}  // namespace

auto main() -> int {
    using howdy::pam::testing::helper_output_value;

    bool ok = true;

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
