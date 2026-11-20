#include "protocol/runtime_descriptor_message.hpp"
#include "test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <span>
#include <vector>

#include <sys/socket.h>

// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" auto LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) -> int;

// Runs the fuzz entry in an ordinary build: well-formed seeds, single-byte mutations of them, and
// fixed-seed random inputs. The entry aborts when the decoder accepts something malformed.
namespace {

	// A fuzz input for a message with `state` and `descriptors` in one SCM_RIGHTS header.
	auto Seed(char state, const std::vector<int> &descriptors) -> std::vector<std::uint8_t> {
		std::vector<std::uint8_t> input{0, 2, 'D', static_cast<std::uint8_t>(state)};
		const std::size_t         bytes = sizeof(int) * descriptors.size();
		alignas(cmsghdr) std::array<std::byte, 64> control{};
		cmsghdr                                    header{};
		header.cmsg_len   = CMSG_LEN(bytes);
		header.cmsg_level = SOL_SOCKET;
		header.cmsg_type  = SCM_RIGHTS;
		std::memcpy(control.data(), &header, sizeof(header));
		std::memcpy(std::span(control).subspan(CMSG_LEN(0)).data(), descriptors.data(), bytes);
		for (std::size_t index = 0; index < CMSG_SPACE(bytes); ++index) {
			input.push_back(std::to_integer<std::uint8_t>(control.at(index)));
		}
		return input;
	}

	auto Accepts(const std::vector<std::uint8_t> &input) -> bool {
		namespace message = howdy::native::runtime_descriptor_message;
		const auto bytes  = std::as_bytes(std::span(input));
		const auto rest   = bytes.subspan(2);
		return message::DecodeMessage({.payload = rest.first(2), .control = rest.subspan(2)}).valid;
	}

}  // namespace

auto main() -> int {
	using howdy::test::Expect;
	const std::array seeds{Seed('R', {5, 6}), Seed('M', {5}), Seed('U', {5}), Seed('I', {5})};
	bool             ok = true;
	for (const auto &seed : seeds) {
		ok &= Expect(Accepts(seed), "a well-formed seed decodes");
		(void)LLVMFuzzerTestOneInput(seed.data(), seed.size());
		for (std::size_t index = 0; index < seed.size(); ++index) {
			for (const std::uint8_t value :
			     std::to_array<std::uint8_t>({0x00, 0x01, 0x7f, 0x80, 0xff})) {
				auto mutated      = seed;
				mutated.at(index) = value;
				(void)LLVMFuzzerTestOneInput(mutated.data(), mutated.size());
			}
		}
	}

	// A fixed seed keeps the replay reproducible; the real fuzzer explores beyond it.
	// NOLINTNEXTLINE(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp)
	std::mt19937                            generator(0x6f70656eU);
	std::uniform_int_distribution<int>      byte(0, 255);
	std::uniform_int_distribution<unsigned> length(0, 160);
	for (int round = 0; round < 50000; ++round) {
		std::vector<std::uint8_t> input(length(generator));
		for (auto &value : input) {
			value = static_cast<std::uint8_t>(byte(generator));
		}
		(void)LLVMFuzzerTestOneInput(input.data(), input.size());
	}
	return ok ? 0 : 1;
}
