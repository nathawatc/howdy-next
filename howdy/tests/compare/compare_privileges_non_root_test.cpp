#include "compare/compare_privileges_test_support.hpp"

auto RunComparePrivilegesNonRootTests() -> bool;

namespace howdy::test::compare_privileges {
	namespace {

		auto TestNonRoot() -> bool {
			bool ok = true;

			FakePrivilegeContext context;
			SetNonRootIdentity(context);
			const auto result = Drop(context);
			ok &= Expect(result.Ok(), "matching non-root credentials succeed");
			ok &= ExpectEvents(context, ExpectedNonRootEvents(), "non-root sequence is exact");
			ok &= Expect(context.lookup_calls == 0, "non-root process does not resolve nobody");
			ok &= Expect(context.capset_header_valid && context.capset_data_zero,
			             "non-root capset clears all capability sets");
			ok &= Expect(context.capget_header_valid, "non-root capget verifies capability sets");
			ok &= Expect(context.regain_uid == 0, "non-root root-regain probe requests UID zero");

			for (const auto &[uids, gids, label] :
			     std::vector<std::tuple<std::array<uid_t, 3>, std::array<gid_t, 3>, std::string>>{
			         {{0, 1000, 1000}, {1000, 1000, 1000}, "real UID zero"},
			         {{1000, 1000, 0}, {1000, 1000, 1000}, "saved UID zero"},
			         {{1000, 1000, 1000}, {0, 1000, 1000}, "real GID zero"},
			         {{1000, 1000, 1000}, {1000, 0, 1000}, "effective GID zero"},
			         {{1000, 1000, 1000}, {1000, 1000, 0}, "saved GID zero"},
			         {{1000, 1001, 1000}, {1000, 1000, 1000}, "mismatched UID slots"},
			         {{1000, 1000, 1000}, {1000, 1001, 1000}, "mismatched GID slots"}}) {
				FakePrivilegeContext invalid_context;
				invalid_context.uids      = uids;
				invalid_context.gids      = gids;
				const auto invalid_result = Drop(invalid_context);
				ok &= VerifyFatalResult(invalid_context, invalid_result,
				                        {"getresuid", "getresgid", "fatal"}, label);
			}

			for (const auto &[groups, label] :
			     std::vector<std::pair<std::vector<gid_t>, std::string>>{
			         {{}, "zero supplementary groups"},
			         {{44}, "one supplementary group"},
			         {{10, 44, 998}, "multiple supplementary groups"},
			         {{1000}, "effective GID duplicated in supplementary groups"}}) {
				FakePrivilegeContext group_context;
				SetNonRootIdentity(group_context);
				group_context.supplementary_groups = groups;
				const auto group_result            = Drop(group_context);
				ok &= Expect(group_result.Ok(), label + " are preserved");
				ok &= ExpectEvents(group_context, ExpectedNonRootEvents(),
				                   label + " preserve non-root verification order");
				ok &= Expect(group_context.group_count == 1 &&
				                 group_context.group_pointer == &group_pointer_sentinel,
				             label + " do not invoke setgroups");
			}

			for (const auto &[fsuid, fsgid, events, label] :
			     std::vector<std::tuple<uid_t, gid_t, std::vector<std::string>, std::string>>{
			         {0, 1000, {"getresuid", "getresgid", "query fsuid"}, "root fsuid"},
			         {1001,
			          1000,
			          {"getresuid", "getresgid", "query fsuid"},
			          "mismatched nonzero fsuid"},
			         {1000,
			          0,
			          {"getresuid", "getresgid", "query fsuid", "query fsgid"},
			          "root fsgid"},
			         {1000,
			          1001,
			          {"getresuid", "getresgid", "query fsuid", "query fsgid"},
			          "mismatched nonzero fsgid"}}) {
				FakePrivilegeContext filesystem_context;
				SetNonRootIdentity(filesystem_context);
				filesystem_context.fsuid     = fsuid;
				filesystem_context.fsgid     = fsgid;
				const auto filesystem_result = Drop(filesystem_context);
				auto       expected          = events;
				expected.emplace_back("fatal");
				ok &= VerifyFatalResult(filesystem_context, filesystem_result, expected, label);
			}

			for (const auto &[capability, label] : kInitiallyFatalCapabilityCases) {
				FakePrivilegeContext capability_context;
				SetNonRootIdentity(capability_context);
				SetCapability(capability_context.capabilities, capability);
				const auto capability_result = Drop(capability_context);
				ok &= VerifyFatalResult(capability_context, capability_result,
				                        {"getresuid", "getresgid", "query fsuid", "query fsgid",
				                         "read capability sets", "fatal"},
				                        std::string("initial non-root ") + label + " capability");
			}

			for (const auto &[capability, label] : kCapabilityCases) {
				FakePrivilegeContext residual_context;
				SetNonRootIdentity(residual_context);
				SetCapability(residual_context.capabilities_after_capset, capability);
				const auto residual_result = Drop(residual_context);
				ok &= VerifyFatalResult(residual_context, residual_result,
				                        {"getresuid", "getresgid", "query fsuid", "query fsgid",
				                         "read capability sets", "clear ambient capabilities",
				                         "clear capability sets", "read capability sets", "fatal"},
				                        std::string("residual ") + label + " capability");
			}

			for (const auto &[failure, events, label] :
			     std::vector<std::tuple<FailureOperation, std::vector<std::string>, std::string>>{
			         {FailureOperation::kInitialCapget,
			          {"getresuid", "getresgid", "query fsuid", "query fsgid",
			           "read capability sets", "fatal"},
			          "initial capability inspection failure"},
			         {FailureOperation::kFinalCapget,
			          {"getresuid", "getresgid", "query fsuid", "query fsgid",
			           "read capability sets", "clear ambient capabilities",
			           "clear capability sets", "read capability sets", "fatal"},
			          "final capability inspection failure"}}) {
				FakePrivilegeContext capability_context;
				SetNonRootIdentity(capability_context);
				capability_context.failure   = failure;
				const auto capability_result = Drop(capability_context);
				ok &= VerifyFatalResult(capability_context, capability_result, events, label);
			}

			for (const auto &[failure, suffix, label] :
			     std::vector<std::tuple<FailureOperation, std::vector<std::string>, std::string>>{
			         {FailureOperation::kAmbient, {"fatal"}, "ambient-capability clear failure"},
			         {FailureOperation::kCapset,
			          {"clear capability sets", "fatal"},
			          "capability-clear failure"},
			         {FailureOperation::kRegainSucceeds,
			          {"clear capability sets", "read capability sets", "root-regain probe",
			           "fatal"},
			          "successful non-root root regain"}}) {
				FakePrivilegeContext failure_context;
				SetNonRootIdentity(failure_context);
				failure_context.failure   = failure;
				const auto failure_result = Drop(failure_context);
				auto       expected       = std::vector<std::string>{"getresuid",
				                                                     "getresgid",
				                                                     "query fsuid",
				                                                     "query fsgid",
				                                                     "read capability sets",
				                                                     "clear ambient capabilities"};
				expected.insert(expected.end(), suffix.begin(), suffix.end());
				ok &= VerifyFatalResult(failure_context, failure_result, expected, label);
			}
			return ok;
		}

		auto TestWaylockInheritableCapability() -> bool {
			FakePrivilegeContext context;
			SetNonRootIdentity(context);
			SetWakeAlarmInheritable(context.capabilities);
			const auto result = Drop(context);

			bool ok = true;
			ok &= Expect(result.Ok(), "Waylock inheritable-only capability sanitizes successfully");
			ok &= Expect(context.fatal_calls == 0,
			             "Waylock inheritable-only capability does not invoke fatal callback");
			ok &= Expect(std::count(context.events.begin(), context.events.end(),
			                        "clear ambient capabilities") == 1,
			             "Waylock inheritable-only capability clears ambient capabilities");
			ok &= Expect(context.capset_calls == 1,
			             "Waylock inheritable-only capability clears capability sets once");
			ok &= Expect(context.capset_header_valid && context.capset_data_zero,
			             "Waylock inheritable-only capability capset zeroes all sets");
			ok &= Expect(context.capget_calls == 2,
			             "Waylock inheritable-only capability performs final capget verification");
			ok &= Expect(context.regain_uid == 0,
			             "Waylock inheritable-only capability performs root-regain probe");
			ok &= ExpectEvents(
			    context, ExpectedNonRootEvents(),
			    "Waylock inheritable-only capability continues through processing gate");
			return ok;
		}

		auto TestWaylockInheritableSanitizationFailures() -> bool {
			struct FailureCase {
				FailureOperation         failure;
				bool                     retain_inheritable;
				std::vector<std::string> events;
				const char              *label;
			};

			const std::array<FailureCase, 3> cases{{
			    {.failure            = FailureOperation::kAmbient,
			     .retain_inheritable = false,
			     .events = {"getresuid", "getresgid", "query fsuid", "query fsgid",
			                "read capability sets", "clear ambient capabilities", "fatal"},
			     .label  = "ambient-clear failure"},
			    {.failure            = FailureOperation::kCapset,
			     .retain_inheritable = false,
			     .events             = {"getresuid", "getresgid", "query fsuid", "query fsgid",
			                            "read capability sets", "clear ambient capabilities",
			                            "clear capability sets", "fatal"},
			     .label              = "capset failure"},
			    {.failure            = FailureOperation::kNone,
			     .retain_inheritable = true,
			     .events             = {"getresuid", "getresgid", "query fsuid", "query fsgid",
			                            "read capability sets", "clear ambient capabilities",
			                            "clear capability sets", "read capability sets", "fatal"},
			     .label              = "residual inheritable capability"},
			}};

			bool ok = true;
			for (const auto &test_case : cases) {
				FakePrivilegeContext context;
				SetNonRootIdentity(context);
				SetWakeAlarmInheritable(context.capabilities);
				context.failure = test_case.failure;
				if (test_case.retain_inheritable) {
					SetWakeAlarmInheritable(context.capabilities_after_capset);
				}
				const auto result = Drop(context);
				ok &= VerifyFatalResult(context, result, test_case.events,
				                        std::string("Waylock inheritable-only ") + test_case.label);
			}
			return ok;
		}

		auto TestInitialCredentialInspectionFailures() -> bool {
			bool ok = true;
			for (const auto &[failure, events, label] :
			     std::vector<std::tuple<FailureOperation, std::vector<std::string>, std::string>>{
			         {FailureOperation::kInitialGetresuid, {"getresuid"}, "initial UID inspection"},
			         {FailureOperation::kInitialGetresgid,
			          {"getresuid", "getresgid"},
			          "initial GID inspection"}}) {
				FakePrivilegeContext context;
				SetNonRootIdentity(context);
				context.failure     = failure;
				const auto result   = Drop(context);
				auto       expected = events;
				expected.emplace_back("fatal");
				ok &= VerifyFatalResult(context, result, expected, label);
				ok &= Expect(context.lookup_calls == 0, label + " does not resolve nobody");
			}
			return ok;
		}

	}  // namespace

	inline auto RunComparePrivilegesNonRootTestsImpl() -> bool {
		bool ok = true;
		ok &= TestNonRoot();
		ok &= TestWaylockInheritableCapability();
		ok &= TestWaylockInheritableSanitizationFailures();
		ok &= TestInitialCredentialInspectionFailures();
		return ok;
	}

}  // namespace howdy::test::compare_privileges

auto RunComparePrivilegesNonRootTests() -> bool {
	return howdy::test::compare_privileges::RunComparePrivilegesNonRootTestsImpl();
}
