#include "cli/clear_cli.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

	auto write_file(const std::filesystem::path &path, const std::string &content) -> bool {
		std::ofstream out(path);
		if (!out.is_open()) {
			return false;
		}
		out << content;
		return out.good();
	}

	auto expect(bool condition, const std::string &message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << "\n";
			return false;
		}
		return true;
	}

	auto run_clear(const std::string &user) -> int {
		std::array<char *, 4> argv{
		    const_cast<char *>("howdy-clear"),
		    const_cast<char *>(user.c_str()),
		    const_cast<char *>("-y"),
		    nullptr,
		};
		return clear_main(3, argv.data());
	}

}  // namespace

auto main() -> int {
	namespace fs = std::filesystem;

	bool            ok         = true;
	const auto      temp_root  = fs::current_path() / "howdy-clear-cli-test";
	const auto      models_dir = temp_root / "models";
	const auto      model_path = models_dir / "alice.dat";
	std::error_code ec;

	fs::remove_all(temp_root, ec);
	fs::create_directories(models_dir, ec);
	ok &= expect(!ec, "create models dir");
	setenv("HOWDY_USER_MODELS_DIR", models_dir.c_str(), 1);

	ok &= expect(write_file(model_path, "not-json"), "write malformed model JSON");
	ok &= expect(run_clear("alice") == 0, "CLI clear removes malformed JSON");
	ok &= expect(!fs::exists(model_path), "malformed JSON model deleted");

	std::string oversized_json = R"json([{"id":1,"label":"large","data":[[0.1]],"padding":")json";
	oversized_json.append((1024 * 1024) + 1, 'x');
	oversized_json += "\"}]";
	ok &= expect(write_file(model_path, oversized_json), "write oversized model JSON");
	ok &= expect(run_clear("alice") == 0, "CLI clear removes oversized JSON");
	ok &= expect(!fs::exists(model_path), "oversized JSON model deleted");

	ok &= expect(write_file(model_path, R"({"id":1})"), "write wrong-shape model JSON");
	ok &= expect(run_clear("alice") == 0, "CLI clear removes wrong-shape JSON");
	ok &= expect(!fs::exists(model_path), "wrong-shape JSON model deleted");

	fs::remove_all(temp_root, ec);
	unsetenv("HOWDY_USER_MODELS_DIR");

	return ok ? 0 : 1;
}
