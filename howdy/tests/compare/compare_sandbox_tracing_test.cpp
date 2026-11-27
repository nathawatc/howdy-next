#include "compare/compare_sandbox_test_groups.hpp"
#include "compare/sandbox.hpp"
#include "compare/sandbox/internal.hpp"
#include "support/prctl_types.hpp"
#include "test_support.hpp"

#include <cerrno>
#include <cstdint>
#include <fstream>
#include <latch>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/prctl.h>
#include <sys/types.h>

#include <linux/prctl.h>

namespace howdy::test::compare_sandbox_tracing {
	namespace {

		using howdy::native::CompareSandboxResource;
		using howdy::native::CompareSandboxResult;
		using howdy::native::CompareSandboxStatus;
		using howdy::native::compare_sandbox_internal::CompareTracingDependencies;
		using howdy::native::compare_sandbox_internal::ParseTracerPid;

		enum class TracingFailure : std::uint8_t {
			kNone,
			kPrctl,
			kFindTracerPid,
		};

		struct FakeTracingContext {
			std::vector<std::string> events;
			TracingFailure           failure    = TracingFailure::kNone;
			pid_t                    tracer_pid = 0;
		};

		auto FakePrctl(void *raw_context, int operation, howdy::native::PrctlArgument argument2,
		               howdy::native::PrctlArgument argument3,
		               howdy::native::PrctlArgument argument4,
		               howdy::native::PrctlArgument argument5) -> int {
			auto &context = *static_cast<FakeTracingContext *>(raw_context);
			if (operation != PR_SET_DUMPABLE || argument2 != 0 || argument3 != 0 ||
			    argument4 != 0 || argument5 != 0) {
				context.events.emplace_back("prctl unexpected");
				errno = EINVAL;
				return -1;
			}
			context.events.emplace_back("prctl dumpable");
			if (context.failure == TracingFailure::kPrctl) {
				errno = EACCES;
				return -1;
			}
			return 0;
		}

		auto FakeFindTracerPid(void *raw_context, pid_t *tracer_pid) -> int {
			auto &context = *static_cast<FakeTracingContext *>(raw_context);
			context.events.emplace_back("read tracer");
			if (context.failure == TracingFailure::kFindTracerPid) {
				errno = ENOENT;
				return -1;
			}
			*tracer_pid = context.tracer_pid;
			return 0;
		}

		auto Disable(FakeTracingContext &context) -> CompareSandboxResult {
			return howdy::native::compare_sandbox_internal::DisableCompareTracing({
			    .context         = &context,
			    .prctl           = FakePrctl,
			    .find_tracer_pid = FakeFindTracerPid,
			});
		}

		auto ExpectFailure(const CompareSandboxResult &result, CompareSandboxStatus status,
		                   int error_number, const std::string &label) -> bool {
			return Expect(result.status == status &&
			                  result.resource == CompareSandboxResource::kNone &&
			                  result.error_number == error_number,
			              label);
		}

		auto TestUntracedSucceeds() -> bool {
			FakeTracingContext context;
			const auto         result = Disable(context);
			bool               ok     = true;
			ok &= Expect(result.status == CompareSandboxStatus::kOk, "untraced compare succeeds");
			ok &=
			    Expect(context.events == std::vector<std::string>{"prctl dumpable", "read tracer"},
				       "dumpable is cleared before the tracer check");
			return ok;
		}

		auto TestFailuresStop() -> bool {
			bool ok = true;

			FakeTracingContext prctl_context;
			prctl_context.failure = TracingFailure::kPrctl;
			ok &= ExpectFailure(Disable(prctl_context), CompareSandboxStatus::kDumpableFailure,
			                    EACCES,
			                    "PR_SET_DUMPABLE failure reports dumpable failure with errno");
			ok &= Expect(prctl_context.events == std::vector<std::string>{"prctl dumpable"},
			             "no tracer check after dumpable failure");

			FakeTracingContext read_context;
			read_context.failure = TracingFailure::kFindTracerPid;
			ok &=
			    ExpectFailure(Disable(read_context), CompareSandboxStatus::kTracerInspectionFailure,
				              ENOENT, "unreadable tracer state fails closed with errno");

			FakeTracingContext traced_context;
			traced_context.tracer_pid = 4242;
			ok &= ExpectFailure(Disable(traced_context), CompareSandboxStatus::kTracerAttached, 0,
			                    "an attached tracer fails closed");

			ok &= ExpectFailure(howdy::native::compare_sandbox_internal::DisableCompareTracing(
			                        CompareTracingDependencies{}),
			                    CompareSandboxStatus::kInvalidDependencies, EINVAL,
			                    "missing tracing dependencies fail closed");
			return ok;
		}

		auto TestParseTracerPid() -> bool {
			struct Case {
				std::string_view status;
				bool             parsed;
				pid_t            tracer_pid;
			};

			bool ok = true;
			for (const auto &test_case : std::vector<Case>{
			         {
			             .status     = "Name:\tcompare\nPPid:\t1\nTracerPid:\t0\nUid:\t0\n",
			             .parsed     = true,
			             .tracer_pid = 0,
			         },
			         {
			             .status     = "Name:\tcompare\nTracerPid:\t31337\n",
			             .parsed     = true,
			             .tracer_pid = 31337,
			         },
			         {
			             .status     = "TracerPid: 7",
			             .parsed     = true,
			             .tracer_pid = 7,
			         },
			         {
			             .status     = "Name:\tcompare\nPPid:\t1\n",
			             .parsed     = false,
			             .tracer_pid = -1,
			         },
			         {
			             .status     = "TracerPid:\t\n",
			             .parsed     = false,
			             .tracer_pid = -1,
			         },
			         {
			             .status     = "TracerPid:\t12x\n",
			             .parsed     = false,
			             .tracer_pid = -1,
			         },
			         {
			             .status     = "TracerPid:\t-1\n",
			             .parsed     = false,
			             .tracer_pid = -1,
			         },
			         {
			             .status     = "XTracerPid:\t0\n",
			             .parsed     = false,
			             .tracer_pid = -1,
			         },
			     }) {
				pid_t      tracer_pid = -1;
				const bool parsed     = ParseTracerPid(test_case.status, &tracer_pid);
				ok &= Expect(parsed == test_case.parsed && tracer_pid == test_case.tracer_pid,
				             "parse TracerPid from: " + std::string(test_case.status));
			}
			return ok;
		}

		// Lets the real check pass under strace or gdb, where kTracerAttached is the right answer.
		auto ProcessIsTraced() -> bool {
			const std::ifstream file("/proc/self/status");
			std::ostringstream  status;
			status << file.rdbuf();
			pid_t tracer_pid = 0;
			return ParseTracerPid(status.str(), &tracer_pid) && tracer_pid != 0;
		}

		auto TestRealProcessIsNonDumpable() -> bool {
			// Later groups in this binary should keep core dumps, so put dumpable back.
			const int previous = prctl(PR_GET_DUMPABLE, 0, 0, 0, 0);
			// A second live thread makes the walk cover more than the main task.
			std::latch  done(1);
			std::thread worker([&done] -> void {
				done.wait();
			});
			const auto  result  = howdy::native::DisableCompareTracing();
			const int   cleared = prctl(PR_GET_DUMPABLE, 0, 0, 0, 0);
			done.count_down();
			worker.join();
			if (previous > 0) {
				(void)prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);
			}
			bool       ok       = true;
			const auto expected = ProcessIsTraced() ? CompareSandboxStatus::kTracerAttached
			                                        : CompareSandboxStatus::kOk;
			ok &= Expect(result.status == expected,
			             "real tracing hardening matches whether the test process is traced");
			ok &= Expect(cleared == 0, "real tracing hardening clears dumpable");
			return ok;
		}

		auto RunTests() -> bool {
			bool ok = true;
			ok &= TestUntracedSucceeds();
			ok &= TestFailuresStop();
			ok &= TestParseTracerPid();
			ok &= TestRealProcessIsNonDumpable();
			return ok;
		}

	}  // namespace

}  // namespace howdy::test::compare_sandbox_tracing

auto RunCompareSandboxTracingTests() -> bool {
	return howdy::test::compare_sandbox_tracing::RunTests();
}
