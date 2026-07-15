#include "common/compare_sandbox.hpp"
#include "common/compare_sandbox_internal.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <sys/prctl.h>
#include <sys/resource.h>

namespace {

	using howdy::native::CompareSandboxResource;
	using howdy::native::CompareSandboxResult;
	using howdy::native::CompareSandboxStatus;
	using howdy::native::compare_sandbox_internal::CompareSandboxDependencies;
	using howdy::native::compare_sandbox_internal::RlimitResource;

	constexpr rlim_t kGibibyte = static_cast<rlim_t>(1024ULL * 1024ULL * 1024ULL);

	constexpr auto make_limit(rlim_t soft, rlim_t hard) -> rlimit {
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
		    {RLIMIT_CPU, make_limit(RLIM_INFINITY, RLIM_INFINITY)},
		    {RLIMIT_NOFILE, make_limit(RLIM_INFINITY, RLIM_INFINITY)},
		    {RLIMIT_CORE, make_limit(RLIM_INFINITY, RLIM_INFINITY)},
		    {RLIMIT_AS, make_limit(RLIM_INFINITY, RLIM_INFINITY)},
		};
		std::map<RlimitResource, rlimit> applied;
		std::vector<std::string>         events;
		FailureOperation                 failure          = FailureOperation::kNone;
		RlimitResource                   failure_resource = RLIMIT_CPU;
	};

	auto resource_name(RlimitResource resource) -> const char * {
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

	auto fake_getrlimit(void *raw_context, RlimitResource resource, rlimit *limit) -> int {
		auto &context = *static_cast<FakeSandboxContext *>(raw_context);
		context.events.emplace_back(std::string("get ") + resource_name(resource));
		if (context.failure == FailureOperation::kGetrlimit &&
		    context.failure_resource == resource) {
			errno = EIO;
			return -1;
		}
		*limit = context.inherited.at(resource);
		return 0;
	}

	auto fake_setrlimit(void *raw_context, RlimitResource resource, const rlimit *limit) -> int {
		auto &context = *static_cast<FakeSandboxContext *>(raw_context);
		context.events.emplace_back(std::string("set ") + resource_name(resource));
		if (context.failure == FailureOperation::kSetrlimit &&
		    context.failure_resource == resource) {
			errno = EPERM;
			return -1;
		}
		context.applied[resource] = *limit;
		return 0;
	}

	auto fake_prctl(void *raw_context, int operation, unsigned long argument2,
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

	auto make_dependencies(FakeSandboxContext &context) -> CompareSandboxDependencies {
		return {
		    .context   = &context,
		    .getrlimit = fake_getrlimit,
		    .setrlimit = fake_setrlimit,
		    .prctl     = fake_prctl,
		};
	}

	auto apply(FakeSandboxContext &context, int timeout_seconds = 4) -> CompareSandboxResult {
		return howdy::native::compare_sandbox_internal::apply_compare_sandbox(
		    timeout_seconds, make_dependencies(context));
	}

	auto expect(bool condition, const std::string &message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << "\n";
			return false;
		}
		return true;
	}

	auto expect_limit(const FakeSandboxContext &context, RlimitResource resource, rlim_t soft,
	                  rlim_t hard, const std::string &message) -> bool {
		const auto found = context.applied.find(resource);
		return expect(found != context.applied.end() && found->second.rlim_cur == soft &&
		                  found->second.rlim_max == hard,
		              message);
	}

	auto expected_success_events() -> std::vector<std::string> {
		return {"prctl",    "get cpu",  "set cpu", "get nofile", "set nofile",
		        "get core", "set core", "get as",  "set as"};
	}

	auto test_preferred_values_accepted() -> bool {
		FakeSandboxContext context;
		context.inherited = {
		    {RLIMIT_CPU, make_limit(100, 100)},
		    {RLIMIT_NOFILE, make_limit(128, 128)},
		    {RLIMIT_CORE, make_limit(1024, 1024)},
		    {RLIMIT_AS, make_limit(4 * kGibibyte, 4 * kGibibyte)},
		};

		const auto result = apply(context);
		bool       ok     = true;
		ok &= expect(result.status == CompareSandboxStatus::kOk, "accept preferred limits");
		ok &= expect_limit(context, RLIMIT_CPU, 15, 20, "apply preferred CPU limit");
		ok &= expect_limit(context, RLIMIT_NOFILE, 32, 32, "apply preferred open-file limit");
		ok &= expect_limit(context, RLIMIT_CORE, 0, 0, "disable core dumps");
		ok &= expect_limit(context, RLIMIT_AS, 5 * kGibibyte / 2, 5 * kGibibyte / 2,
		                   "apply preferred address-space limit");
		ok &= expect(context.events == expected_success_events(), "preserve syscall order");
		return ok;
	}

	auto test_unlimited_values_accepted() -> bool {
		FakeSandboxContext context;
		const auto         result = apply(context);
		bool               ok     = true;
		ok &= expect(result.status == CompareSandboxStatus::kOk, "accept unlimited inheritance");
		ok &= expect_limit(context, RLIMIT_CPU, 15, 20, "finite CPU limit from infinity");
		ok &= expect_limit(context, RLIMIT_NOFILE, 32, 32, "finite open-file limit from infinity");
		ok &= expect_limit(context, RLIMIT_CORE, 0, 0, "finite core limit from infinity");
		ok &= expect_limit(context, RLIMIT_AS, 5 * kGibibyte / 2, 5 * kGibibyte / 2,
		                   "finite address-space limit from infinity");
		return ok;
	}

	auto test_lower_hard_limits_clamped() -> bool {
		bool ok = true;
		{
			FakeSandboxContext context;
			context.inherited[RLIMIT_CPU] = make_limit(17, 17);
			ok &= expect(apply(context).status == CompareSandboxStatus::kOk,
			             "accept lower usable CPU hard limit");
			ok &= expect_limit(context, RLIMIT_CPU, 15, 17, "clamp CPU hard limit");
		}
		{
			FakeSandboxContext context;
			context.inherited[RLIMIT_NOFILE] = make_limit(24, 24);
			ok &= expect(apply(context).status == CompareSandboxStatus::kOk,
			             "accept lower usable open-file hard limit");
			ok &= expect_limit(context, RLIMIT_NOFILE, 24, 24, "clamp open-file hard limit");
		}
		{
			FakeSandboxContext context;
			const rlim_t       inherited = 9 * kGibibyte / 4;
			context.inherited[RLIMIT_AS] = make_limit(inherited, inherited);
			ok &= expect(apply(context).status == CompareSandboxStatus::kOk,
			             "accept lower usable address-space hard limit");
			ok &= expect_limit(context, RLIMIT_AS, inherited, inherited,
			                   "clamp address-space hard limit");
		}
		return ok;
	}

	auto test_lower_soft_limits_preserved() -> bool {
		bool ok = true;
		{
			FakeSandboxContext context;
			context.inherited[RLIMIT_CPU] = make_limit(10, 100);
			ok &= expect(apply(context).status == CompareSandboxStatus::kOk,
			             "accept lower usable CPU soft limit");
			ok &= expect_limit(context, RLIMIT_CPU, 10, 20, "preserve CPU soft limit");
		}
		{
			FakeSandboxContext context;
			context.inherited[RLIMIT_NOFILE] = make_limit(24, 100);
			ok &= expect(apply(context).status == CompareSandboxStatus::kOk,
			             "accept lower usable open-file soft limit");
			ok &= expect_limit(context, RLIMIT_NOFILE, 24, 32, "preserve open-file soft limit");
		}
		{
			FakeSandboxContext context;
			const rlim_t       inherited = 9 * kGibibyte / 4;
			context.inherited[RLIMIT_AS] = make_limit(inherited, 4 * kGibibyte);
			ok &= expect(apply(context).status == CompareSandboxStatus::kOk,
			             "accept lower usable address-space soft limit");
			ok &= expect_limit(context, RLIMIT_AS, inherited, 5 * kGibibyte / 2,
			                   "preserve address-space soft limit");
		}
		return ok;
	}

	auto test_mixed_finite_and_infinite_limits() -> bool {
		bool ok = true;
		{
			FakeSandboxContext context;
			context.inherited[RLIMIT_CPU] = make_limit(10, RLIM_INFINITY);
			ok &= expect(apply(context).status == CompareSandboxStatus::kOk,
			             "accept finite CPU soft and infinite hard");
			ok &= expect_limit(context, RLIMIT_CPU, 10, 20, "mixed CPU finite soft target");
		}
		{
			FakeSandboxContext context;
			context.inherited[RLIMIT_NOFILE] = make_limit(RLIM_INFINITY, 24);
			ok &= expect(apply(context).status == CompareSandboxStatus::kOk,
			             "accept infinite open-file soft and finite hard");
			ok &= expect_limit(context, RLIMIT_NOFILE, 24, 24,
			                   "clamp open-file soft to inherited hard");
		}
		{
			FakeSandboxContext context;
			const rlim_t       inherited = 9 * kGibibyte / 4;
			context.inherited[RLIMIT_AS] = make_limit(RLIM_INFINITY, inherited);
			ok &= expect(apply(context).status == CompareSandboxStatus::kOk,
			             "accept infinite address-space soft and finite hard");
			ok &= expect_limit(context, RLIMIT_AS, inherited, inherited,
			                   "clamp address-space soft to inherited hard");
		}
		return ok;
	}

	auto expected_resource(RlimitResource resource) -> CompareSandboxResource {
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

	auto expected_prefix_through_get(RlimitResource resource) -> std::vector<std::string> {
		auto       events = expected_success_events();
		const auto marker = std::string("get ") + resource_name(resource);
		const auto found  = std::ranges::find(events, marker);
		events.erase(found + 1, events.end());
		return events;
	}

	auto expected_prefix_through_set(RlimitResource resource) -> std::vector<std::string> {
		auto       events = expected_success_events();
		const auto marker = std::string("set ") + resource_name(resource);
		const auto found  = std::ranges::find(events, marker);
		events.erase(found + 1, events.end());
		return events;
	}

	auto test_below_minimum_stops() -> bool {
		struct Case {
			RlimitResource resource;
			rlimit         inherited;
		};

		const std::vector<Case> cases{
		    {.resource = RLIMIT_CPU, .inherited = make_limit(8, 100)},
		    {.resource = RLIMIT_NOFILE, .inherited = make_limit(15, 100)},
		    {.resource = RLIMIT_AS, .inherited = make_limit((2 * kGibibyte) - 1, 4 * kGibibyte)},
		};

		bool ok = true;
		for (const auto &test_case : cases) {
			FakeSandboxContext context;
			context.inherited[test_case.resource] = test_case.inherited;
			const auto result                     = apply(context);
			ok &= expect(result.status == CompareSandboxStatus::kLimitBelowMinimum,
			             std::string("reject below-minimum ") + resource_name(test_case.resource));
			ok &= expect(result.resource == expected_resource(test_case.resource),
			             std::string("report below-minimum ") + resource_name(test_case.resource));
			ok &= expect(!context.applied.contains(test_case.resource),
			             std::string("do not apply below-minimum ") +
			                 resource_name(test_case.resource));
			ok &= expect(context.events == expected_prefix_through_get(test_case.resource),
			             std::string("stop after below-minimum ") +
			                 resource_name(test_case.resource));
		}
		return ok;
	}

	auto test_inspection_failures_stop() -> bool {
		const std::vector<RlimitResource> resources{RLIMIT_CPU, RLIMIT_NOFILE, RLIMIT_CORE,
		                                            RLIMIT_AS};
		bool                              ok = true;
		for (const auto resource : resources) {
			FakeSandboxContext context;
			context.failure          = FailureOperation::kGetrlimit;
			context.failure_resource = resource;
			const auto result        = apply(context);
			ok &= expect(result.status == CompareSandboxStatus::kLimitInspectionFailure,
			             std::string("report inspection failure for ") + resource_name(resource));
			ok &=
			    expect(result.resource == expected_resource(resource) && result.error_number == EIO,
			           std::string("include inspection resource and errno for ") +
			               resource_name(resource));
			ok &= expect(!context.applied.contains(resource),
			             std::string("do not apply uninspected ") + resource_name(resource));
			ok &=
			    expect(context.events == expected_prefix_through_get(resource),
			           std::string("stop after inspection failure for ") + resource_name(resource));
		}
		return ok;
	}

	auto test_application_failures_stop() -> bool {
		const std::vector<RlimitResource> resources{RLIMIT_CPU, RLIMIT_NOFILE, RLIMIT_CORE,
		                                            RLIMIT_AS};
		bool                              ok = true;
		for (const auto resource : resources) {
			FakeSandboxContext context;
			context.failure          = FailureOperation::kSetrlimit;
			context.failure_resource = resource;
			const auto result        = apply(context);
			ok &= expect(result.status == CompareSandboxStatus::kLimitApplicationFailure,
			             std::string("report application failure for ") + resource_name(resource));
			ok &= expect(result.resource == expected_resource(resource) &&
			                 result.error_number == EPERM,
			             std::string("include application resource and errno for ") +
			                 resource_name(resource));
			ok &= expect(!context.applied.contains(resource),
			             std::string("do not record failed application for ") +
			                 resource_name(resource));
			ok &= expect(context.events == expected_prefix_through_set(resource),
			             std::string("stop after application failure for ") +
			                 resource_name(resource));
		}
		return ok;
	}

	auto test_no_new_privileges_failure_stops() -> bool {
		FakeSandboxContext context;
		context.failure   = FailureOperation::kPrctl;
		const auto result = apply(context);
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

	auto test_timeout_derived_cpu_arithmetic() -> bool {
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
			const auto         result = apply(context, test_case.timeout);
			ok &= expect(result.status == CompareSandboxStatus::kOk, test_case.name);
			ok &= expect_limit(context, RLIMIT_CPU, test_case.expected_soft,
			                   test_case.expected_hard, test_case.name);
		}
		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= test_preferred_values_accepted();
	ok &= test_unlimited_values_accepted();
	ok &= test_lower_hard_limits_clamped();
	ok &= test_lower_soft_limits_preserved();
	ok &= test_mixed_finite_and_infinite_limits();
	ok &= test_below_minimum_stops();
	ok &= test_inspection_failures_stop();
	ok &= test_application_failures_stop();
	ok &= test_no_new_privileges_failure_stops();
	ok &= test_timeout_derived_cpu_arithmetic();
	return ok ? 0 : 1;
}
