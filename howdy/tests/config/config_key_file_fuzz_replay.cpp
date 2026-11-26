#include "../../src/config/config_utils/internal.hpp"
#include "config/config_key_file_fuzz.hpp"
#include "test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

// Runs the fuzz check in an ordinary build: shapes that broke the editor before, and fixed-seed
// random inputs. The check aborts when inih reads an update differently than it promised.
namespace {

	using howdy::native::config_utils_internal::ConfigLineReplaceResult;
	using howdy::test::CheckKeyFileReplacement;
	using howdy::test::Expect;
	using howdy::test::KeyFileFuzzTarget;
	using howdy::test::RunKeyFileFuzzInput;

	// The config byte decoded as the target's section header.
	constexpr std::uint8_t kSectionToken = 0x81;

	struct Seed {
		std::string_view        content;
		ConfigLineReplaceResult expected = ConfigLineReplaceResult::kReplaced;
	};

	auto CheckSeeds(const KeyFileFuzzTarget &target, const std::vector<Seed> &seeds) -> bool {
		bool ok = true;
		for (const auto &seed : seeds) {
			ok &= Expect(CheckKeyFileReplacement(target, seed.content) == seed.expected,
			             "seed: " + std::string(seed.content));
		}
		return ok;
	}

	auto CheckSwitchSeeds() -> bool {
		constexpr auto ambiguous = ConfigLineReplaceResult::kAmbiguousContinuation;
		constexpr auto duplicate = ConfigLineReplaceResult::kDuplicate;
		constexpr auto not_found = ConfigLineReplaceResult::kNotFound;
		return CheckSeeds(
		    {.section = "core", .key = "disabled", .value = "true"},
		    {
		        {.content = "[core]\ndisabled = false\n"},
		        {.content = "[core]\ndisabled = false"},
		        // Continuation lines after blank and comment lines.
		        {.content = "[core]\ndisabled = false\n\n; note\n\n  second\ntimeout = 4\n"},
		        {.content = "[core]\ndisabled = false\n  # note\n  second\n"},
		        {
		            .content  = "[core]\ndisabled = false\n  abort_if_ssh = true\n",
		            .expected = ambiguous,
		        },
		        {
		            .content  = "[core]\ndisabled = false\n\n  [video]\n",
		            .expected = ambiguous,
		        },
		        // Options without a separator own nothing until the rewrite.
		        {.content = "[core]\ndisabled true\n"},
		        {.content = "[core]\ndisabled"},
		        {.content = "[core]\ndisabled ; a = b\n"},
		        {
		            .content  = "[core]\nabort_if_ssh = true\ndisabled\n  second\n",
		            .expected = ambiguous,
		        },
		        {.content = "[core]\ndisabled = false\nmalformed\n  second\n"},
		        // Sections that are not closed change neither the section nor the previous name.
		        {.content = "[core]\ndisabled = false\n[video\n  second\ntimeout = 3\n"},
		        {.content = "[core\n[core]\ndisabled = false\n"},
		        {
		            .content  = "[core\ndisabled = false\n",
		            .expected = not_found,
		        },
		        // An empty name ends the previous option, so an indented line stands on its own.
		        {.content = "[core]\ndisabled = false\n= x\n  abort_if_ssh = true\n"},
		        {.content = "[core]\n= x\n  disabled = false\n  second\n"},
		        // Inline comments.
		        {.content = "[core] ; [video]\ndisabled = false ; note = old\n  second ; x = y\n"},
		        {
		            .content  = "[core ;]\ndisabled = false\n",
		            .expected = not_found,
		        },
		        {.content = "[core]\ndisabled;x = 1\ndisabled = false\n"},
		        // Indentation with every whitespace character inih accepts.
		        {.content = "[core]\ndisabled = false\n\fsecond\n\v third\n\tfourth\n\f\n"},
		        {.content = "[core]\ndisabled\ffalse\n"},
		        // Line ends and a byte order mark.
		        {.content = "[core]\r\ndisabled = false\r\n  second\r\n\r\nx = 1\r\n"},
		        {.content = "\xEF\xBB\xBF[core]\ndisabled = false\n"},
		        {.content = "\xEF\xBB\xBF  [core]\n  disabled = false\n"},
		        {.content = "[CORE]\nDISABLED: false\n"},
		        {
		            .content  = "[core]\ndisabled = false\n[core]\ndisabled = false\n",
		            .expected = duplicate,
		        },
		        {
		            .content  = "[video]\ndisabled = false\n",
		            .expected = not_found,
		        },
		    });
	}

	auto CheckDevicePathSeeds() -> bool {
		return CheckSeeds(
		    {
		        .section = "video",
		        .key     = "device_path",
		        .value   = "/dev/v4l/by-path/pci-0000:00:14.0-usb-0:1:1.0-video-index0",
		    },
		    {
		        {.content = "[video]\ndevice_path = none\ntimeout = 4\n"},
		        {.content = "[video]\ndevice_path: /dev/v4l/by-path/pci-0000:00:14.0\n"},
		        {.content = "[video]\ndevice_path =\n  /dev/v4l/by-path/pci-0000:00:14.0=x\n"},
		        {
		            .content  = "[video]\ndevice_path = a\n  pci-0000:00:14.0\n",
		            .expected = ConfigLineReplaceResult::kAmbiguousContinuation,
		        },
		    });
	}

	struct RandomReplay {
		std::uint32_t seed            = 0;
		unsigned      min_config_byte = 0;  // 0x80 for tokens only
	};

	// Returns how many random inputs were replaced.
	auto ReplayRandomInputs(const RandomReplay &replay) -> int {
		constexpr int rounds = 40000;
		// NOLINTNEXTLINE(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp)
		std::mt19937 generator(replay.seed);
		// Not a standard distribution, whose output differs between libraries.
		const auto below = [&generator](unsigned limit) -> unsigned {
			return static_cast<unsigned>(generator() % limit);
		};
		int replaced = 0;
		for (int round = 0; round < rounds; ++round) {
			constexpr std::size_t     value_bytes = 4;
			std::vector<std::uint8_t> input{
			    static_cast<std::uint8_t>(below(256)),
			    value_bytes,
			};
			for (std::size_t index = 0; index < value_bytes; ++index) {
				input.push_back(static_cast<std::uint8_t>(below(256)));
			}
			// Half the configs open the target's section, which random tokens rarely do first.
			if (round % 2 == 0) {
				input.push_back(kSectionToken);
			}
			for (auto remaining = below(41); remaining > 0; --remaining) {
				input.push_back(static_cast<std::uint8_t>(replay.min_config_byte +
				                                          below(256 - replay.min_config_byte)));
			}
			if (RunKeyFileFuzzInput(input) == ConfigLineReplaceResult::kReplaced) {
				++replaced;
			}
		}
		return replaced;
	}

}  // namespace

auto main() -> int {
	bool ok = CheckSwitchSeeds();
	ok &= CheckDevicePathSeeds();

	// Tokens only, then tokens mixed with literal bytes. The floors keep a change to the decoding
	// from leaving the replay with nothing to check.
	const int from_tokens = ReplayRandomInputs({.seed = 0x696e6921U, .min_config_byte = 0x80});
	const int from_bytes  = ReplayRandomInputs({.seed = 0x696e6922U});
	std::cout << "replaced: " << from_tokens << " from tokens, " << from_bytes << " from bytes\n";
	ok &= Expect(from_tokens > 2500, "token inputs replace often enough to check");
	ok &= Expect(from_bytes > 2500, "byte inputs replace often enough to check");
	return ok ? 0 : 1;
}
