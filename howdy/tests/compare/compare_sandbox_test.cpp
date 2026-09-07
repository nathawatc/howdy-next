#include "compare/sandbox.hpp"
#include "compare/sandbox/internal.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <sys/prctl.h>
#include <sys/resource.h>

namespace {

	using howdy::test::expect;

	using howdy::native::CompareSandboxResource;
	using howdy::native::CompareSandboxResult;
	using howdy::native::CompareSandboxStatus;
	using howdy::native::compare_sandbox_internal::CompareSandboxDependencies;
	using howdy::native::compare_sandbox_internal::RlimitResource;

	constexpr rlim_t kGibibyte = static_cast<rlim_t>(1024ULL * 1024ULL * 1024ULL);

	constexpr auto MakeLimit(rlim_t soft, rlim_t hard) -> rlimit {
		return {
		    .rlim_cur = soft,
		    .rlim_max = hard,
		};
	}

	enum class FailureOperation : std::uint8_t {
		kNone,
		kPrctl,
		kGetrlimit,
		kSetrlimit,
	};

	struct FakeSandboxContext {
		std::map<RlimitResource, rlimit> inherited{
		    {RLIMIT_CPU, MakeLimit(RLIM_INFINITY, RLIM_INFINITY)},
		    {RLIMIT_NOFILE, MakeLimit(RLIM_INFINITY, RLIM_INFINITY)},
		    {RLIMIT_CORE, MakeLimit(RLIM_INFINITY, RLIM_INFINITY)},
		    {RLIMIT_AS, MakeLimit(RLIM_INFINITY, RLIM_INFINITY)},
		};
		std::map<RlimitResource, rlimit> applied;
		std::vector<std::string>         events;
		FailureOperation                 failure          = FailureOperation::kNone;
		RlimitResource                   failure_resource = RLIMIT_CPU;
	};

	auto ResourceName(RlimitResource resource) -> const char * {
		switch (resource) {
			case RLIMIT_CPU:
				return "cpu";
			case RLIMIT_NOFILE:
				return "nofile";
			case RLIMIT_CORE:
				return "core";
			case RLIMIT_AS:
				return "as";
			default:
				return "unknown";
		}
	}

	auto FakeGetrlimit(void *raw_context, RlimitResource resource, rlimit *limit) -> int {
		auto &context = *static_cast<FakeSandboxContext *>(raw_context);
		context.events.emplace_back(std::string("get ") + ResourceName(resource));
		if (context.failure == FailureOperation::kGetrlimit &&
		    context.failure_resource == resource) {
			errno = EIO;
			return -1;
		}
		*limit = context.inherited.at(resource);
		return 0;
	}

	auto FakeSetrlimit(void *raw_context, RlimitResource resource, const rlimit *limit) -> int {
		auto &context = *static_cast<FakeSandboxContext *>(raw_context);
		context.events.emplace_back(std::string("set ") + ResourceName(resource));
		if (context.failure == FailureOperation::kSetrlimit &&
		    context.failure_resource == resource) {
			errno = EPERM;
			return -1;
		}
		context.applied[resource] = *limit;
		return 0;
	}

	auto FakePrctl(void *raw_context, int operation, unsigned long argument2,
	               unsigned long argument3, unsigned long argument4, unsigned long argument5)
	    -> int {
		auto &context = *static_cast<FakeSandboxContext *>(raw_context);
		context.events.emplace_back("prctl");
		if (operation != PR_SET_NO_NEW_PRIVS || argument2 != 1 || argument3 != 0 ||
		    argument4 != 0 || argument5 != 0 || context.failure == FailureOperation::kPrctl) {
			errno = EACCES;
			return -1;
		}
		return 0;
	}

	auto MakeDependencies(FakeSandboxContext &context) -> CompareSandboxDependencies {
		return {
		    .context   = &context,
		    .getrlimit = FakeGetrlimit,
		    .setrlimit = FakeSetrlimit,
		    .prctl     = FakePrctl,
		};
	}

	auto Apply(FakeSandboxContext &context, int timeout_seconds = 4) -> CompareSandboxResult {
		return howdy::native::compare_sandbox_internal::ApplyCompareSandbox(
		    timeout_seconds, MakeDependencies(context));
	}

	auto ExpectLimit(const FakeSandboxContext &context, RlimitResource resource, rlim_t soft,
	                 rlim_t hard, const std::string &message) -> bool {
		const auto found = context.applied.find(resource);
		return expect(found != context.applied.end() && found->second.rlim_cur == soft &&
		                  found->second.rlim_max == hard,
		              message);
	}

	auto ExpectedSuccessEvents() -> std::vector<std::string> {
		return {"prctl",    "get cpu",  "set cpu", "get nofile", "set nofile",
		        "get core", "set core", "get as",  "set as"};
	}

	auto TestPreferredValuesAccepted() -> bool {
		FakeSandboxContext context;
		context.inherited = {
		    {RLIMIT_CPU, MakeLimit(100, 100)},
		    {RLIMIT_NOFILE, MakeLimit(128, 128)},
		    {RLIMIT_CORE, MakeLimit(1024, 1024)},
		    {RLIMIT_AS, MakeLimit(4 * kGibibyte, 4 * kGibibyte)},
		};

		const auto result = Apply(context);
		bool       ok     = true;
		ok &= expect(result.status == CompareSandboxStatus::kOk, "accept preferred limits");
		ok &= ExpectLimit(context, RLIMIT_CPU, 15, 20, "apply preferred CPU limit");
		ok &= ExpectLimit(context, RLIMIT_NOFILE, 32, 32, "apply preferred open-file limit");
		ok &= ExpectLimit(context, RLIMIT_CORE, 0, 0, "disable core dumps");
		ok &= ExpectLimit(context, RLIMIT_AS, 5 * kGibibyte / 2, 5 * kGibibyte / 2,
		                  "apply preferred address-space limit");
		ok &= expect(context.events == ExpectedSuccessEvents(), "preserve syscall order");
		return ok;
	}

	auto TestUnlimitedValuesAccepted() -> bool {
		FakeSandboxContext context;
		const auto         result = Apply(context);
		bool               ok     = true;
		ok &= expect(result.status == CompareSandboxStatus::kOk, "accept unlimited inheritance");
		ok &= ExpectLimit(context, RLIMIT_CPU, 15, 20, "finite CPU limit from infinity");
		ok &= ExpectLimit(context, RLIMIT_NOFILE, 32, 32, "finite open-file limit from infinity");
		ok &= ExpectLimit(context, RLIMIT_CORE, 0, 0, "finite core limit from infinity");
		ok &= ExpectLimit(context, RLIMIT_AS, 5 * kGibibyte / 2, 5 * kGibibyte / 2,
		                  "finite address-space limit from infinity");
		return ok;
	}

	auto TestLowerHardLimitsClamped() -> bool {
		bool ok = true;
		{
			FakeSandboxContext context;
			context.inherited[RLIMIT_CPU] = MakeLimit(17, 17);
			ok &= expect(Apply(context).status == CompareSandboxStatus::kOk,
			             "accept lower usable CPU hard limit");
			ok &= ExpectLimit(context, RLIMIT_CPU, 15, 17, "clamp CPU hard limit");
		}
		{
			FakeSandboxContext context;
			context.inherited[RLIMIT_NOFILE] = MakeLimit(24, 24);
			ok &= expect(Apply(context).status == CompareSandboxStatus::kOk,
			             "accept lower usable open-file hard limit");
			ok &= ExpectLimit(context, RLIMIT_NOFILE, 24, 24, "clamp open-file hard limit");
		}
		{
			FakeSandboxContext context;
			const rlim_t       inherited = 9 * kGibibyte / 4;
			context.inherited[RLIMIT_AS] = MakeLimit(inherited, inherited);
			ok &= expect(Apply(context).status == CompareSandboxStatus::kOk,
			             "accept lower usable address-space hard limit");
			ok &= ExpectLimit(context, RLIMIT_AS, inherited, inherited,
			                  "clamp address-space hard limit");
		}
		return ok;
	}

	auto TestLowerSoftLimitsPreserved() -> bool {
		bool ok = true;
		{
			FakeSandboxContext context;
			context.inherited[RLIMIT_CPU] = MakeLimit(10, 100);
			ok &= expect(Apply(context).status == CompareSandboxStatus::kOk,
			             "accept lower usable CPU soft limit");
			ok &= ExpectLimit(context, RLIMIT_CPU, 10, 20, "preserve CPU soft limit");
		}
		{
			FakeSandboxContext context;
			context.inherited[RLIMIT_NOFILE] = MakeLimit(24, 100);
			ok &= expect(Apply(context).status == CompareSandboxStatus::kOk,
			             "accept lower usable open-file soft limit");
			ok &= ExpectLimit(context, RLIMIT_NOFILE, 24, 32, "preserve open-file soft limit");
		}
		{
			FakeSandboxContext context;
			const rlim_t       inherited = 9 * kGibibyte / 4;
			context.inherited[RLIMIT_AS] = MakeLimit(inherited, 4 * kGibibyte);
			ok &= expect(Apply(context).status == CompareSandboxStatus::kOk,
			             "accept lower usable address-space soft limit");
			ok &= ExpectLimit(context, RLIMIT_AS, inherited, 5 * kGibibyte / 2,
			                  "preserve address-space soft limit");
		}
		return ok;
	}

	auto TestMixedFiniteAndInfiniteLimits() -> bool {
		bool ok = true;
		{
			FakeSandboxContext context;
			context.inherited[RLIMIT_CPU] = MakeLimit(10, RLIM_INFINITY);
			ok &= expect(Apply(context).status == CompareSandboxStatus::kOk,
			             "accept finite CPU soft and infinite hard");
			ok &= ExpectLimit(context, RLIMIT_CPU, 10, 20, "mixed CPU finite soft target");
		}
		{
			FakeSandboxContext context;
			context.inherited[RLIMIT_NOFILE] = MakeLimit(RLIM_INFINITY, 24);
			ok &= expect(Apply(context).status == CompareSandboxStatus::kOk,
			             "accept infinite open-file soft and finite hard");
			ok &= ExpectLimit(context, RLIMIT_NOFILE, 24, 24,
			                  "clamp open-file soft to inherited hard");
		}
		{
			FakeSandboxContext context;
			const rlim_t       inherited = 9 * kGibibyte / 4;
			context.inherited[RLIMIT_AS] = MakeLimit(RLIM_INFINITY, inherited);
			ok &= expect(Apply(context).status == CompareSandboxStatus::kOk,
			             "accept infinite address-space soft and finite hard");
			ok &= ExpectLimit(context, RLIMIT_AS, inherited, inherited,
			                  "clamp address-space soft to inherited hard");
		}
		return ok;
	}

	auto ExpectedResource(RlimitResource resource) -> CompareSandboxResource {
		switch (resource) {
			case RLIMIT_CPU:
				return CompareSandboxResource::kCpu;
			case RLIMIT_NOFILE:
				return CompareSandboxResource::kOpenFiles;
			case RLIMIT_CORE:
				return CompareSandboxResource::kCore;
			case RLIMIT_AS:
				return CompareSandboxResource::kAddressSpace;
			default:
				return CompareSandboxResource::kNone;
		}
	}

	auto ExpectedPrefixThroughGet(RlimitResource resource) -> std::vector<std::string> {
		auto       events = ExpectedSuccessEvents();
		const auto marker = std::string("get ") + ResourceName(resource);
		const auto found  = std::ranges::find(events, marker);
		events.erase(found + 1, events.end());
		return events;
	}

	auto ExpectedPrefixThroughSet(RlimitResource resource) -> std::vector<std::string> {
		auto       events = ExpectedSuccessEvents();
		const auto marker = std::string("set ") + ResourceName(resource);
		const auto found  = std::ranges::find(events, marker);
		events.erase(found + 1, events.end());
		return events;
	}

	auto TestBelowMinimumStops() -> bool {
		struct Case {
			RlimitResource resource;
			rlimit         inherited;
		};

		const std::vector<Case> cases{
		    {.resource = RLIMIT_CPU, .inherited = MakeLimit(8, 100)},
		    {.resource = RLIMIT_NOFILE, .inherited = MakeLimit(15, 100)},
		    {.resource = RLIMIT_AS, .inherited = MakeLimit((2 * kGibibyte) - 1, 4 * kGibibyte)},
		};

		bool ok = true;
		for (const auto &test_case : cases) {
			FakeSandboxContext context;
			context.inherited[test_case.resource] = test_case.inherited;
			const auto result                     = Apply(context);
			ok &= expect(result.status == CompareSandboxStatus::kLimitBelowMinimum,
			             std::string("reject below-minimum ") + ResourceName(test_case.resource));
			ok &= expect(result.resource == ExpectedResource(test_case.resource),
			             std::string("report below-minimum ") + ResourceName(test_case.resource));
			ok &= expect(!context.applied.contains(test_case.resource),
			             std::string("do not apply below-minimum ") +
			                 ResourceName(test_case.resource));
			ok &=
			    expect(context.events == ExpectedPrefixThroughGet(test_case.resource),
			           std::string("stop after below-minimum ") + ResourceName(test_case.resource));
		}
		return ok;
	}

	auto TestInspectionFailuresStop() -> bool {
		const std::vector<RlimitResource> resources{RLIMIT_CPU, RLIMIT_NOFILE, RLIMIT_CORE,
		                                            RLIMIT_AS};
		bool                              ok = true;
		for (const auto resource : resources) {
			FakeSandboxContext context;
			context.failure          = FailureOperation::kGetrlimit;
			context.failure_resource = resource;
			const auto result        = Apply(context);
			ok &= expect(result.status == CompareSandboxStatus::kLimitInspectionFailure,
			             std::string("report inspection failure for ") + ResourceName(resource));
			ok &= expect(
			    result.resource == ExpectedResource(resource) && result.error_number == EIO,
			    std::string("include inspection resource and errno for ") + ResourceName(resource));
			ok &= expect(!context.applied.contains(resource),
			             std::string("do not apply uninspected ") + ResourceName(resource));
			ok &=
			    expect(context.events == ExpectedPrefixThroughGet(resource),
			           std::string("stop after inspection failure for ") + ResourceName(resource));
		}
		return ok;
	}

	auto TestApplicationFailuresStop() -> bool {
		const std::vector<RlimitResource> resources{RLIMIT_CPU, RLIMIT_NOFILE, RLIMIT_CORE,
		                                            RLIMIT_AS};
		bool                              ok = true;
		for (const auto resource : resources) {
			FakeSandboxContext context;
			context.failure          = FailureOperation::kSetrlimit;
			context.failure_resource = resource;
			const auto result        = Apply(context);
			ok &= expect(result.status == CompareSandboxStatus::kLimitApplicationFailure,
			             std::string("report application failure for ") + ResourceName(resource));
			ok &= expect(result.resource == ExpectedResource(resource) &&
			                 result.error_number == EPERM,
			             std::string("include application resource and errno for ") +
			                 ResourceName(resource));
			ok &= expect(!context.applied.contains(resource),
			             std::string("do not record failed application for ") +
			                 ResourceName(resource));
			ok &=
			    expect(context.events == ExpectedPrefixThroughSet(resource),
			           std::string("stop after application failure for ") + ResourceName(resource));
		}
		return ok;
	}

	auto TestNoNewPrivilegesFailureStops() -> bool {
		FakeSandboxContext context;
		context.failure   = FailureOperation::kPrctl;
		const auto result = Apply(context);
		bool       ok     = true;
		ok &= expect(result.status == CompareSandboxStatus::kNoNewPrivilegesFailure,
		             "report no_new_privs failure");
		ok &= expect(result.resource == CompareSandboxResource::kNone &&
		                 result.error_number == EACCES,
		             "include no_new_privs errno");
		ok &= expect(context.events == std::vector<std::string>{"prctl"},
		             "no limit calls after no_new_privs failure");
		ok &= expect(context.applied.empty(), "no limits applied after no_new_privs failure");
		return ok;
	}

	auto TestTimeoutDerivedCpuArithmetic() -> bool {
		struct Case {
			int         timeout;
			rlim_t      expected_soft;
			rlim_t      expected_hard;
			const char *name;
		};

		const std::vector<Case> cases{
		    {.timeout = 1, .expected_soft = 15, .expected_hard = 20, .name = "timeout below floor"},
		    {.timeout       = 30,
		     .expected_soft = 35,
		     .expected_hard = 40,
		     .name          = "timeout above floor"},
		    {.timeout       = 300,
		     .expected_soft = 305,
		     .expected_hard = 310,
		     .name          = "large valid timeout"},
		    {.timeout       = INT_MAX,
		     .expected_soft = static_cast<rlim_t>(INT_MAX) + 5,
		     .expected_hard = static_cast<rlim_t>(INT_MAX) + 10,
		     .name          = "overflow-safe timeout"},
		};

		bool ok = true;
		for (const auto &test_case : cases) {
			FakeSandboxContext context;
			const auto         result = Apply(context, test_case.timeout);
			ok &= expect(result.status == CompareSandboxStatus::kOk, test_case.name);
			ok &= ExpectLimit(context, RLIMIT_CPU, test_case.expected_soft, test_case.expected_hard,
			                  test_case.name);
		}
		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= TestPreferredValuesAccepted();
	ok &= TestUnlimitedValuesAccepted();
	ok &= TestLowerHardLimitsClamped();
	ok &= TestLowerSoftLimitsPreserved();
	ok &= TestMixedFiniteAndInfiniteLimits();
	ok &= TestBelowMinimumStops();
	ok &= TestInspectionFailuresStop();
	ok &= TestApplicationFailuresStop();
	ok &= TestNoNewPrivilegesFailureStops();
	ok &= TestTimeoutDerivedCpuArithmetic();
	return ok ? 0 : 1;
}
