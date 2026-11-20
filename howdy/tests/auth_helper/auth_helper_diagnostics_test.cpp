#include "auth_helper/auth_helper_test_groups.hpp"
#include "auth_helper/command.hpp"
#include "support/scoped_stream_capture.hpp"
#include "test_support.hpp"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
	using howdy::test::Expect;

	auto ExpectQuietDiagnostics() -> bool {
		using howdy::native::ScopedStreamCapture;
		using howdy::native::auth_helper::command::RunWithQuietDiagnostics;

		const auto *const original = std::cerr.rdbuf();
		bool              ok       = true;
		bool              unwound  = false;
		try {
			const ScopedStreamCapture capture(std::cerr);
			throw std::runtime_error("unwind");
		} catch (const std::runtime_error &) {
			unwound = true;
		}
		ok &= Expect(unwound && std::cerr.rdbuf() == original,
		             "stream capture restores cerr while unwinding");

		std::string reported;
		const auto  run = [&](const std::function<bool()> &operation) -> std::string {
			reported.clear();
			ScopedStreamCapture outer(std::cerr);
			try {
				(void)RunWithQuietDiagnostics(operation, [&](std::string_view line) -> void {
					reported += std::string(line) + "|";
				});
			} catch (const std::runtime_error &) {
				std::cerr << "[rethrown]";
			}
			return outer.Release();
		};
		ok &= Expect(run([] -> bool {
			             std::cerr << "open diagnostic\n";
			             return true;
		             }).empty() &&
		                 reported == "open diagnostic|",
		             "diagnostics are logged, not written to stderr, when the operation succeeds");
		ok &= Expect(run([&] -> bool {
			             std::cerr << "first\nsecond";
			             const bool logged_live = reported == "first|";
			             std::cerr << "\n";
			             return !logged_live;
		             }) == "first\nsecond\n" &&
		                 reported == "first|second|",
		             "each line is logged as it is written, before the operation returns");
		ok &= Expect(run([] -> bool {
			             std::cerr << "no newline";
			             return true;
		             }).empty() &&
		                 reported == "no newline|",
		             "a last line without a newline is still logged on success");
		ok &= Expect(run([] -> bool {
			             std::cerr << "open diagnostic\n";
			             throw std::runtime_error("open failed");
		             }) == "open diagnostic\n[rethrown]" &&
		                 reported == "open diagnostic|",
		             "diagnostics reach stderr before an exception propagates");
		return ok;
	}

}  // namespace

auto RunAuthHelperDiagnosticsTests() -> bool {
	return ExpectQuietDiagnostics();
}
