#include "integration/process.hpp"

#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

	auto Require(bool condition, const std::string &message) -> void {
		if (!condition) {
			throw std::runtime_error(message);
		}
	}

	auto Read(const fs::path &path) -> std::string {
		std::ifstream input(path, std::ios::binary);
		Require(input.good(), "cannot read: " + path.string());
		return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	}

	auto RequireNonempty(const fs::path &path) -> std::string {
		const auto content = Read(path);
		Require(!content.empty(), "missing or empty generated documentation: " + path.string());
		return content;
	}

	auto Contains(const std::string &content, std::string_view text, const fs::path &path) -> void {
		Require(content.contains(text), "missing '" + std::string(text) + "' in " + path.string());
	}

	auto ExactlyOnce(const std::string &content, std::string_view text, const fs::path &path)
	    -> void {
		std::size_t count = 0;
		std::size_t start = 0;
		while ((start = content.find(text, start)) != std::string::npos) {
			++count;
			start += text.size();
		}
		Require(count == 1, "expected one '" + std::string(text) + "' in " + path.string() +
		                        ", found " + std::to_string(count));
	}

	auto RoffPath(const std::string &path) -> std::string {
		std::string escaped;
		for (const char character : path) {
			if (character == '-') {
				escaped += '\\';
			}
			escaped += character;
		}
		return escaped;
	}

	auto Date(const std::string &content, std::string_view title, int section) -> std::string {
		const std::regex header("^\\.TH " + std::string(title) + " " + std::to_string(section) +
		                        " \"([^\"]*)\"");
		std::smatch      match;
		Require(std::regex_search(content, match, header),
		        "missing man-page date: " + std::string(title));
		return match.str(1);
	}

	auto CheckSyntax(const fs::path &man1, const fs::path &man5, const fs::path &man8) -> void {
		for (const auto &path : {man1, man5, man8}) {
			auto result =
			    howdy::test::integration::RunProcess({"mandoc", "-T", "utf8", path.string()});
			if (result.status == 127) {
				result = howdy::test::integration::RunProcess(
				    {"groff", "-man", "-Tutf8", path.string()});
			}
			if (result.status == 127) {
				std::cout << "Man syntax check skipped: no mandoc or groff available\n";
				return;
			}
			Require(result.status == 0,
			        "man syntax check failed: " + path.string() + ": " + result.output);
		}
	}

}  // namespace

auto main(int argc, char **argv) -> int {
	if (argc != 11) {
		std::cerr << "usage: docs_test BUILD SOURCE MAN1 MAN5 MAN8 CONFIG_DIR CONFIG_PATH "
		             "MODELS_DIR USER_MODELS_DIR AUTH_HELPER\n";
		return 2;
	}
	try {
		const auto     args             = howdy::test::integration::CopyArguments(argc, argv);
		const fs::path build            = args.at(1);
		const fs::path source           = args.at(2);
		const fs::path man1             = args.at(3);
		const fs::path man5             = args.at(4);
		const fs::path man8             = args.at(5);
		const auto     man1_text        = RequireNonempty(man1);
		const auto     man5_text        = RequireNonempty(man5);
		const auto     man8_text        = RequireNonempty(man8);
		const auto     refs             = build / "generated/docs";
		const auto     commands_path    = refs / "howdy-commands.roff";
		const auto     options_path     = refs / "howdy-options.roff";
		const auto     workarounds_path = refs / "pam-workarounds.roff";
		const auto     ini_path         = refs / "howdy-ini-options.roff";
		const auto     commands         = RequireNonempty(commands_path);
		const auto     options          = RequireNonempty(options_path);
		const auto     workarounds      = RequireNonempty(workarounds_path);
		const auto     ini              = RequireNonempty(ini_path);

		const auto date = Date(man1_text, "HOWDY", 1);
		Require(std::regex_match(date, std::regex("[0-9]{4}-[0-9]{2}-[0-9]{2}")) &&
		            date == Date(man5_text, "HOWDY\\.INI", 5) &&
		            date == Date(man8_text, "PAM_HOWDY", 8),
		        "man pages do not share an ISO UTC date");
		for (const auto &path : {
		         source / "howdy/howdy.1.in",
		         source / "howdy/howdy.ini.5.in",
		         source / "pam/pam_howdy.8.in",
		     }) {
			Require(!std::regex_search(Read(path), std::regex("[0-9]{4}-[0-9]{2}-[0-9]{2}")),
			        "hardcoded calendar date in " + path.string());
		}

		for (const auto *command : {
		         "add [LABEL]",
		         "clear",
		         "config",
		         "disable {0|1|true|false}",
		         "download\\-models",
		         "list",
		         "remove ID",
		         "set KEY VALUE",
		         "snapshot",
		         "test [\\-\\-device DEVICE]",
		         "version",
		     }) {
			const std::string label = "\\&\\fB" + std::string(command) + "\\fR";
			ExactlyOnce(commands, label, commands_path);
			ExactlyOnce(man1_text, label, man1);
		}
		for (const auto *option :
		     {R"(\-U, \-\-user USER)", R"(\-\-plain)", R"(\-y)", R"(\-h, \-\-help)"}) {
			const std::string label = "\\&\\fB" + std::string(option) + "\\fR";
			ExactlyOnce(options, label, options_path);
			ExactlyOnce(man1_text, label, man1);
		}
		for (const auto *value : {"input", "native", "native\\-input"}) {
			const std::string label = R"(\&\fBworkaround=)" + std::string(value) + R"(\fR)";
			ExactlyOnce(workarounds, label, workarounds_path);
			ExactlyOnce(man8_text, label, man8);
		}
		Contains(man8_text, "Workaround is off when workaround= is omitted.", man8);
		Require(!workarounds.contains("workaround=off"), "invalid workaround=off generated");
		for (const auto *section : {"core", "video", "face", "debug"}) {
			const std::string label = ".SS [" + std::string(section) + "]";
			ExactlyOnce(ini, label, ini_path);
			ExactlyOnce(man5_text, label, man5);
		}
		for (const auto *option :
		     {"detection_notice", "timeout", "sface_threshold", "end_report"}) {
			const std::string label = "\\&\\fB" + std::string(option) + "\\fR";
			ExactlyOnce(ini, label, ini_path);
			ExactlyOnce(man5_text, label, man5);
		}
		Contains(man5_text, "Show progress messages for each face scan attempt.", man5);
		Contains(man1_text, "howdy.ini (5)", man1);
		Contains(man8_text, "howdy.ini (5)", man8);
		Contains(man5_text, "howdy (1)", man5);
		Contains(man5_text, "pam_howdy (8)", man5);
		Contains(man1_text, RoffPath(args.at(7)), man1);
		Contains(man1_text, RoffPath(args.at(8) + "/"), man1);
		Contains(man1_text, RoffPath(args.at(9) + "/"), man1);
		Contains(man8_text, RoffPath(args.at(6)), man8);
		Contains(man8_text, RoffPath(args.at(7)), man8);
		Contains(man8_text, RoffPath(args.at(9) + "/"), man8);
		Contains(man8_text, RoffPath(args.at(10)), man8);
		Contains(man5_text, RoffPath(args.at(7)), man5);

		for (const auto &path :
		     {source / "howdy/howdy.1", source / "howdy/howdy.ini.5", source / "pam/pam_howdy.8"}) {
			Require(!fs::exists(path), "checked-in generated man page: " + path.string());
		}
		const auto readme = Read(source / "README.md");
		Require(!std::regex_search(readme, std::regex(R"((^|\n)\|[ \t]*Command[ \t]*\|)")),
		        "README still contains exhaustive command table");
		Contains(readme, "See `howdy(1)` for the complete command and option reference.",
		         source / "README.md");
		Contains(readme, "See `pam_howdy(8)` for PAM configuration and workaround modes.",
		         source / "README.md");
		const auto pam_readme = Read(source / "pam/README.md");
		Require(!std::regex_search(pam_readme,
		                           std::regex("workaround=(off|input|native|native-input)")),
		        "pam/README.md still contains workaround reference values");
		Contains(pam_readme, "pam_howdy(8)", source / "pam/README.md");

		const auto manifest = build / "install_manifest.txt";
		if (fs::is_regular_file(manifest)) {
			const auto text = Read(manifest);
			for (const auto *private_file : {
			         "howdy_docs_generator",
			         "howdy-commands.roff",
			         "howdy-options.roff",
			         "pam-workarounds.roff",
			         "howdy-ini-options.roff",
			     }) {
				Require(!text.contains(private_file),
				        "private documentation artifact in install manifest: " +
				            std::string(private_file));
			}
		}
		CheckSyntax(man1, man5, man8);
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
