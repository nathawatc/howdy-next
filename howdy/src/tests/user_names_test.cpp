#include "common/user_names.hpp"

#include <filesystem>
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
    bool ok = true;

    ok &= expect(howdy::native::is_valid_model_user_name("alice"), "normal username is accepted");
    ok &= expect(howdy::native::is_valid_model_user_name("alice@example.com"),
                 "domain-style username is accepted");
    ok &= expect(!howdy::native::is_valid_model_user_name(""), "empty username is rejected");
    ok &= expect(!howdy::native::is_valid_model_user_name("alice/bob"),
                 "username containing slash is rejected");
    ok &= expect(!howdy::native::is_valid_model_user_name("alice\\bob"),
                 "username containing backslash is rejected");
    ok &= expect(!howdy::native::is_valid_model_user_name("../alice"),
                 "malformed path-traversal username input is rejected");
    ok &= expect(!howdy::native::is_valid_model_user_name("alice..bob"),
                 "malformed username containing dot-dot is rejected");
    ok &= expect(!howdy::native::is_valid_model_user_name(".alice"),
                 "hidden-file style username is rejected");
    ok &= expect(!howdy::native::is_valid_model_user_name("alice bob"),
                 "username containing whitespace is rejected");

    const std::filesystem::path models_dir = "/trusted/models";
    ok &= expect(howdy::native::resolve_user_model_path(models_dir, "alice") ==
                     models_dir / "alice.dat",
                 "safe username resolves inside models directory");
    ok &= expect(!howdy::native::resolve_user_model_path(models_dir, "../alice").has_value(),
                 "unsafe username cannot escape models directory");
    ok &= expect(!howdy::native::resolve_user_model_path(models_dir, "alice..bob").has_value(),
                 "dot-dot username input is rejected before model path construction");

    ok &= expect(howdy::native::is_valid_model_label("Laptop camera"),
                 "normal model label is accepted");
    ok &= expect(howdy::native::is_valid_model_label(""), "empty model label remains accepted");
    ok &= expect(!howdy::native::is_valid_model_label("bad/name"),
                 "model label containing slash is rejected");
    ok &= expect(!howdy::native::is_valid_model_label("bad\\name"),
                 "model label containing backslash is rejected");
    ok &= expect(!howdy::native::is_valid_model_label("bad\nname"),
                 "model label containing newline is rejected");
    ok &= expect(!howdy::native::is_valid_model_label("bad\tname"),
                 "model label containing tab is rejected");

    return ok ? 0 : 1;
}
