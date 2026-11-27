#include "auth_helper/auth_helper_test_groups.hpp"
#include "auth_helper/command.hpp"
#include "support/scoped_stream_capture.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <functional>
#include <ios>
#include <iostream>
#include <new>
#include <stdexcept>
#include <streambuf>
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

	class NullWhatException final : public std::exception {
	public:
		[[nodiscard]] auto what() const noexcept -> const char * override {
			return nullptr;
		}
	};

	class ThrowingStreambuf final : public std::streambuf {
	protected:
		auto overflow(int /*ch*/) -> int override {
			throw std::runtime_error("stderr boom");
		}

		auto xsputn(const char * /*data*/, std::streamsize /*count*/) -> std::streamsize override {
			throw std::runtime_error("stderr boom");
		}
	};

	class ScopedThrowingCerr {
	public:
		ScopedThrowingCerr()
		    : previous_(std::cerr.rdbuf(&throwing_buffer_))
		    , previous_exceptions_(std::cerr.exceptions()) {
			std::cerr.clear();
			std::cerr.exceptions(std::ios::badbit | std::ios::failbit);
		}

		ScopedThrowingCerr(const ScopedThrowingCerr &)                     = delete;
		auto operator=(const ScopedThrowingCerr &) -> ScopedThrowingCerr & = delete;
		ScopedThrowingCerr(ScopedThrowingCerr &&)                          = delete;
		auto operator=(ScopedThrowingCerr &&) -> ScopedThrowingCerr &      = delete;

		~ScopedThrowingCerr() {
			std::cerr.exceptions(std::ios::goodbit);
			std::cerr.rdbuf(previous_);
			std::cerr.clear();
			std::cerr.exceptions(previous_exceptions_);
		}

	private:
		ThrowingStreambuf throwing_buffer_;
		std::streambuf   *previous_;
		std::ios::iostate previous_exceptions_;
	};

	auto ExpectGuardedExceptionTable() -> bool {
		using howdy::native::ScopedStreamCapture;
		using howdy::native::auth_helper::command::RunGuarded;

		constexpr std::size_t expected_fixed_buffer_size = 512;
		bool                  ok                         = true;

		struct GuardCase {
			const char          *label;
			std::function<int()> operation;
			int                  expected_exit;
			std::string_view     expected_prefix;
			std::size_t          min_size;
			std::size_t          max_size;
		};

		const auto cases = std::to_array<GuardCase>({
		    {
		        .label     = "normal success passes through untouched",
		        .operation = [] -> int {
			        return 0;
		        },
		        .expected_exit   = 0,
		        .expected_prefix = "",
		        .min_size        = 0,
		        .max_size        = 0,
		    },
		    {
		        .label     = "std::runtime_error logs message and returns 1",
		        .operation = [] -> int {
			        throw std::runtime_error("config read failed");
		        },
		        .expected_exit   = 1,
		        .expected_prefix = "howdy-auth-helper failed: config read failed",
		        .min_size        = 44,
		        .max_size        = 44,
		    },
		    {
		        .label     = "empty std::runtime_error falls back to unknown error",
		        .operation = [] -> int {
			        throw std::runtime_error("");
		        },
		        .expected_exit   = 1,
		        .expected_prefix = "howdy-auth-helper failed: unknown error",
		        .min_size        = 39,
		        .max_size        = 39,
		    },
		    {
		        .label     = "std::exception with null what() falls back to unknown error",
		        .operation = [] -> int {
			        throw NullWhatException();
		        },
		        .expected_exit   = 1,
		        .expected_prefix = "howdy-auth-helper failed: unknown error",
		        .min_size        = 39,
		        .max_size        = 39,
		    },
		    {
		        .label = "std::bad_alloc logs non-empty detail without allocating and returns 1",
		        .operation = [] -> int {
			        throw std::bad_alloc();
		        },
		        .expected_exit   = 1,
		        .expected_prefix = "howdy-auth-helper failed: ",
		        .min_size        = 27,
		        .max_size        = expected_fixed_buffer_size,
		    },
		    {
		        .label     = "non-std::exception logs unknown error and returns 1",
		        .operation = [] -> int {
			        // NOLINTNEXTLINE(bugprone-std-exception-baseclass)
			        throw 42;
		        },
		        .expected_exit   = 1,
		        .expected_prefix = "howdy-auth-helper failed: unknown error",
		        .min_size        = 39,
		        .max_size        = 39,
		    },
		    {
		        .label     = "overlong exception message truncates cleanly into fixed buffer",
		        .operation = [] -> int {
			        throw std::runtime_error(std::string(1024, 'x'));
		        },
		        .expected_exit   = 1,
		        .expected_prefix = "howdy-auth-helper failed: ",
		        .min_size        = expected_fixed_buffer_size,
		        .max_size        = expected_fixed_buffer_size,
		    },
		    {
		        .label           = "null operation fails closed with diagnostic",
		        .operation       = {},
		        .expected_exit   = 1,
		        .expected_prefix = "howdy-auth-helper failed: missing auth-helper operation",
		        .min_size        = 55,
		        .max_size        = 55,
		    },
		});

		for (const auto &test_case : cases) {
			std::string         logged;
			ScopedStreamCapture outer(std::cerr);
			const int           exit_code =
			    RunGuarded(test_case.operation, [&](std::string_view line) -> void {
				    logged += line;
			    });
			const std::string stderr_output = outer.Release();
			const bool        length_matches =
			    logged.size() >= test_case.min_size && logged.size() <= test_case.max_size;
			const bool        prefix_matches  = logged.starts_with(test_case.expected_prefix);
			const std::string expected_stderr = logged.empty() ? std::string{} : (logged + "\n");
			ok &= Expect(exit_code == test_case.expected_exit && length_matches && prefix_matches &&
			                 stderr_output == expected_stderr,
			             test_case.label);
			if (test_case.min_size == expected_fixed_buffer_size && prefix_matches) {
				const std::string_view suffix =
				    std::string_view(logged).substr(test_case.expected_prefix.size());
				ok &= Expect(std::ranges::all_of(suffix,
				                                 [](char ch) -> bool {
					                                 return ch == 'x';
				                                 }),
				             "truncated overlong exception fills fixed buffer with payload bytes");
			}
		}

		return ok;
	}

	auto ExpectGuardedExceptionEdgeCases() -> bool {
		using howdy::native::ScopedStreamCapture;
		using howdy::native::auth_helper::command::RunGuarded;
		using howdy::native::auth_helper::command::RunWithQuietDiagnostics;

		bool ok = true;

		// A null log callback still reports to stderr and returns 1.
		{
			ScopedStreamCapture outer(std::cerr);
			const int           exit_code = RunGuarded(
			    [] -> int {
				    throw std::runtime_error("helper boom");
			    },
			    std::function<void(std::string_view)>{});
			const std::string stderr_output = outer.Release();
			ok &=
			    Expect(exit_code == 1 && stderr_output == "howdy-auth-helper failed: helper boom\n",
				       "null log callback still reports diagnostic to stderr and returns 1");
		}

		// A throwing log callback must not abort RunGuarded.
		{
			ScopedStreamCapture outer(std::cerr);
			const int           exit_code = RunGuarded(
			    [] -> int {
				    throw std::runtime_error("helper boom");
			    },
			    [](std::string_view) -> void {
				    throw std::runtime_error("logger boom");
			    });
			const std::string stderr_output = outer.Release();
			ok &=
			    Expect(exit_code == 1 && stderr_output == "howdy-auth-helper failed: helper boom\n",
				       "throwing log callback does not prevent stderr report or exit 1");
		}

		// A throwing std::cerr must not abort RunGuarded and still delivers the log diagnostic.
		{
			std::string logged;
			int         exit_code = 0;
			{
				const ScopedThrowingCerr throwing_cerr;
				exit_code = RunGuarded(
				    [] -> int {
					    throw std::runtime_error("helper boom");
				    },
				    [&](std::string_view line) -> void {
					    logged += line;
				    });
			}
			ok &= Expect(exit_code == 1 && logged == "howdy-auth-helper failed: helper boom",
			             "throwing stderr does not abort RunGuarded or suppress log callback");
		}

		// Quiet diagnostics captured before an exception still reach both log and stderr, followed
		// by the caught exception message.
		{
			std::string logged;
			const auto  record = [&](std::string_view line) -> void {
				logged += std::string(line) + "|";
			};
			ScopedStreamCapture outer(std::cerr);
			const int           exit_code = RunGuarded(
			    [&] -> int {
				    return RunWithQuietDiagnostics(
				               [] -> bool {
					               std::cerr << "open diagnostic\n";
					               throw std::runtime_error("open failed");
				               },
				               record)
				               ? 0
							   : 1;
			    },
			    record);
			const std::string stderr_output = outer.Release();
			ok &= Expect(
			    exit_code == 1 &&
			        stderr_output == "open diagnostic\nhowdy-auth-helper failed: open failed\n" &&
			        logged == "open diagnostic|howdy-auth-helper failed: open failed|",
			    "RunGuarded logs uncaught exception after RunWithQuietDiagnostics unwinds");
		}

		return ok;
	}

}  // namespace

auto RunAuthHelperDiagnosticsTests() -> bool {
	return ExpectQuietDiagnostics() && ExpectGuardedExceptionTable() &&
	       ExpectGuardedExceptionEdgeCases();
}
