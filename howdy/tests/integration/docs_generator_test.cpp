#include "integration/process.hpp"

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

	class Cleanup {
	public:
		explicit Cleanup(fs::path root)
		    : root_(std::move(root)) {}

		Cleanup(const Cleanup &)                     = delete;
		auto operator=(const Cleanup &) -> Cleanup & = delete;
		Cleanup(Cleanup &&)                          = delete;
		auto operator=(Cleanup &&) -> Cleanup &      = delete;

		~Cleanup() {
			std::error_code error;
			fs::remove_all(root_, error);
		}

	private:
		fs::path root_;
	};

	auto Require(bool condition, const std::string &message) -> void {
		if (!condition) {
			throw std::runtime_error(message);
		}
	}

}  // namespace

auto main(int argc, char **argv) -> int {
	if (argc < 3) {
		std::cerr << "usage: docs_generator_test GENERATOR ROOT\n";
		return 2;
	}
	try {
		const auto               arguments = howdy::test::integration::CopyArguments(argc, argv);
		const fs::path           generator = arguments.at(1);
		const fs::path           root      = arguments.at(2);
		std::vector<std::string> command(arguments.begin() + 3, arguments.end());
		command.push_back(generator.string());
		fs::remove_all(root);
		fs::create_directories(root);
		const Cleanup cleanup(root);

		const auto invalid = howdy::test::integration::RunProcess(command);
		Require(invalid.status == 2, "invalid invocation returned " +
		                                 std::to_string(invalid.status) + ": " + invalid.output);

		const auto output_dir = root / "output";
		fs::create_directory(output_dir);
		auto generated_command = command;
		generated_command.insert(generated_command.end(), {"--output-dir", output_dir.string()});
		const auto generated = howdy::test::integration::RunProcess(generated_command);
		Require(generated.status == 0, "generation failed: " + generated.output);
		for (const auto *fragment : {
		         "howdy-commands.roff",
		         "howdy-options.roff",
		         "pam-workarounds.roff",
		         "howdy-ini-options.roff",
		     }) {
			const auto path = output_dir / fragment;
			Require(fs::is_regular_file(path) && fs::file_size(path) > 0,
			        "missing or empty generated fragment: " + path.string());
		}

		const auto blocked_parent = root / "blocked";
		std::ofstream(blocked_parent) << "not a directory\n";
		auto blocked_command = command;
		blocked_command.insert(blocked_command.end(),
		                       {"--output-dir", (blocked_parent / "output").string()});
		const auto blocked = howdy::test::integration::RunProcess(blocked_command);
		Require(blocked.status != 0, "filesystem failure unexpectedly succeeded");
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
