#include "compare/compare_privileges_test_support.hpp"

auto run_compare_privileges_privileged_tests() -> bool;

namespace howdy::test::compare_privileges {
	namespace {

		auto test_privileged_success() -> bool {
			FakePrivilegeContext context;
			const auto           result = drop(context);
			bool                 ok     = true;
			ok &= expect(result.ok(), "privileged credential drop succeeds");
			ok &= expect_events(context, expected_privileged_events(),
			                    "privileged sequence is exact");
			ok &= expect(context.lookup_name == "nobody", "lookup resolves exactly nobody");
			ok &= expect(context.group_count == 0 && context.group_pointer == nullptr,
			             "setgroups receives zero and null pointer");
			ok &= expect(context.set_gids == std::array<gid_t, 3>{context.target_gid,
			                                                      context.target_gid,
			                                                      context.target_gid},
			             "setresgid receives resolved GID three times");
			ok &= expect(context.set_uids == std::array<uid_t, 3>{context.target_uid,
			                                                      context.target_uid,
			                                                      context.target_uid},
			             "setresuid receives resolved UID three times");
			ok &= expect(context.capset_header_valid && context.capset_data_zero,
			             "capset uses ABI v3 and zero sets");
			ok &= expect(context.capget_header_valid, "capget uses ABI v3");
			ok &= expect(context.fsuid == context.target_uid, "post-drop fsuid equals nobody UID");
			ok &= expect(context.fsgid == context.target_gid, "post-drop fsgid equals nobody GID");
			ok &= expect(context.getgroups_calls == 1 && context.supplementary_groups.empty(),
			             "post-drop supplementary-group verification observes zero groups");
			ok &= expect(context.regain_uid == 0, "root-regain probe requests UID zero");
			ok &= expect(context.fatal_calls == 0, "successful drop does not terminate");

			for (const auto &[uids, gids, label] :
			     std::vector<std::tuple<std::array<uid_t, 3>, std::array<gid_t, 3>, std::string>>{
			         {{1000, 0, 0}, {1000, 1000, 1000}, "setuid-root credentials"},
			         {{1000, 0, 2000}, {1000, 2000, 3000}, "mixed privileged credentials"}}) {
				FakePrivilegeContext privileged_context;
				privileged_context.uids      = uids;
				privileged_context.gids      = gids;
				const auto privileged_result = drop(privileged_context);
				ok &= expect(privileged_result.ok(), label + " enter privileged drop path");
				ok &= expect_events(privileged_context, expected_privileged_events(),
				                    label + " preserve privileged operation order");
				ok &= expect(privileged_context.uids ==
				                 std::array<uid_t, 3>{privileged_context.target_uid,
				                                      privileged_context.target_uid,
				                                      privileged_context.target_uid},
				             label + " end with nobody UID slots");
				ok &= expect(privileged_context.gids ==
				                 std::array<gid_t, 3>{privileged_context.target_gid,
				                                      privileged_context.target_gid,
				                                      privileged_context.target_gid},
				             label + " end with nobody GID slots");
			}

			FakePrivilegeContext safe_securebits_context;
			safe_securebits_context.securebits = SECBIT_NOROOT;
			const auto safe_securebits_result  = drop(safe_securebits_context);
			ok &= expect(safe_securebits_result.ok(), "unrelated safe securebit remains allowed");
			return ok;
		}

		auto test_privileged_residual_capabilities() -> bool {
			bool ok = true;
			for (const auto &[capability, label] : kCapabilityCases) {
				FakePrivilegeContext context;
				set_capability(context.capabilities_after_capset, capability);
				const auto result = drop(context);
				ok &= verify_fatal_result(
				    context, result,
				    {"getresuid", "getresgid", "lookup", "query securebits",
				     "clear ambient capabilities", "clear supplementary groups", "setresgid",
				     "setresuid", "clear capability sets", "read capability sets", "fatal"},
				    std::string("privileged residual ") + label + " capability");
				ok &= expect(context.lookup_name == "nobody",
				             std::string(label) + " residual capability uses nobody path");
				ok &= expect(context.set_uids == std::array<uid_t, 3>{context.target_uid,
				                                                      context.target_uid,
				                                                      context.target_uid},
				             std::string(label) + " residual capability occurs after setresuid");
			}
			return ok;
		}

		auto test_lookup_and_identity_failures() -> bool {
			bool ok = true;
			for (const auto &[mode, label] : std::vector<std::pair<LookupMode, std::string>>{
			         {LookupMode::kMissing, "missing"}, {LookupMode::kError, "error"}}) {
				FakePrivilegeContext context;
				context.lookup_mode = mode;
				const auto result   = drop(context);
				ok &= expect(result.status == ComparePrivilegeStatus::kLookupFailed,
				             "account " + label + " fails closed");
				ok &= expect_events(context, {"getresuid", "getresgid", "lookup"},
				                    "lookup failure stops all later operations");
			}

			FakePrivilegeContext erange_context;
			erange_context.lookup_mode = LookupMode::kErange;
			const auto erange_result   = drop(erange_context);
			ok &= expect(erange_result.status == ComparePrivilegeStatus::kLookupFailed,
			             "repeated ERANGE fails closed");
			ok &= expect(erange_context.largest_lookup_buffer == std::size_t{64} * 1024,
			             "passwd lookup reaches 64 KiB cap");
			ok &= expect(erange_context.lookup_calls == 7, "passwd retries are bounded");
			ok &= expect(erange_context.events.size() ==
			                 static_cast<std::size_t>(erange_context.lookup_calls) + 2,
			             "ERANGE performs identity inspection and lookup only");

			for (const auto &[root_uid, root_gid, label] :
			     std::vector<std::tuple<bool, bool, std::string>>{{true, false, "UID"},
			                                                      {false, true, "GID"}}) {
				FakePrivilegeContext context;
				context.target_uid = root_uid ? 0 : context.target_uid;
				context.target_gid = root_gid ? 0 : context.target_gid;
				const auto result  = drop(context);
				ok &= expect(result.status == ComparePrivilegeStatus::kInvalidIdentity,
				             "root " + label + " is rejected");
				ok &= expect_events(context, {"getresuid", "getresgid", "lookup"},
				                    "invalid identity stops all later operations");
			}
			return ok;
		}

		auto test_privileged_pre_mutation_failures() -> bool {
			struct FailureCase {
				FailureOperation         failure;
				ComparePrivilegeStatus   status;
				std::vector<std::string> events;
				std::string              label;
			};

			const std::vector<FailureCase> cases{
			    {.failure = FailureOperation::kSecurebits,
			     .status  = ComparePrivilegeStatus::kCapabilityFailure,
			     .events  = {"getresuid", "getresgid", "lookup", "query securebits"},
			     .label   = "securebits query"},
			    {.failure = FailureOperation::kAmbient,
			     .status  = ComparePrivilegeStatus::kCapabilityFailure,
			     .events  = {"getresuid", "getresgid", "lookup", "query securebits",
			                 "clear ambient capabilities"},
			     .label   = "ambient clear"},
			};

			bool ok = true;
			for (const auto &test_case : cases) {
				FakePrivilegeContext context;
				context.failure   = test_case.failure;
				const auto result = drop(context);
				ok &= expect(result.status == test_case.status,
				             test_case.label + " returns structured failure");
				ok &= expect(!result.ok(), test_case.label + " is not success");
				ok &= expect(context.fatal_calls == 0,
				             test_case.label + " does not fatal before UID drop");
				ok &= expect_events(context, test_case.events,
				                    test_case.label + " stops later operations");
			}

			for (const auto &[securebits, label] : std::array<std::pair<int, const char *>, 2>{
			         {{SECBIT_KEEP_CAPS, "KEEP_CAPS"},
			          {SECBIT_NO_SETUID_FIXUP, "NO_SETUID_FIXUP"}}}) {
				FakePrivilegeContext context;
				context.securebits = securebits;
				const auto result  = drop(context);
				ok &= expect(result.status == ComparePrivilegeStatus::kCapabilityFailure,
				             std::string(label) + " is rejected");
				ok &=
				    expect_events(context, {"getresuid", "getresgid", "lookup", "query securebits"},
				                  std::string(label) + " stops before credential mutation");
			}
			return ok;
		}

		auto test_privileged_failure_paths() -> bool {
			const std::vector<std::string> through_setresuid{"getresuid",
			                                                 "getresgid",
			                                                 "lookup",
			                                                 "query securebits",
			                                                 "clear ambient capabilities",
			                                                 "clear supplementary groups",
			                                                 "setresgid",
			                                                 "setresuid"};
			bool                           ok = true;

			for (const auto &[failure, events, label] :
			     std::vector<std::tuple<FailureOperation, std::vector<std::string>, std::string>>{
			         {FailureOperation::kGroups,
			          {"getresuid", "getresgid", "lookup", "query securebits",
			           "clear ambient capabilities", "clear supplementary groups", "fatal"},
			          "supplementary-group clear failure"},
			         {FailureOperation::kSetresgid,
			          {"getresuid", "getresgid", "lookup", "query securebits",
			           "clear ambient capabilities", "clear supplementary groups", "setresgid",
			           "fatal"},
			          "setresgid failure"},
			         {FailureOperation::kSetresuid,
			          {"getresuid", "getresgid", "lookup", "query securebits",
			           "clear ambient capabilities", "clear supplementary groups", "setresgid",
			           "setresuid", "fatal"},
			          "setresuid failure"}}) {
				FakePrivilegeContext context;
				context.failure   = failure;
				const auto result = drop(context);
				ok &= verify_fatal_result(context, result, events, label);
			}

			for (const auto &[failure, suffix, label] :
			     std::vector<std::tuple<FailureOperation, std::vector<std::string>, std::string>>{
			         {FailureOperation::kCapset,
			          {"clear capability sets", "fatal"},
			          "capset failure"},
			         {FailureOperation::kCapget,
			          {"clear capability sets", "read capability sets", "fatal"},
			          "capget failure"},
			         {FailureOperation::kGetresgid,
			          {"clear capability sets", "read capability sets", "getresgid", "fatal"},
			          "GID verification failure"},
			         {FailureOperation::kGetresuid,
			          {"clear capability sets", "read capability sets", "getresgid", "getresuid",
			           "fatal"},
			          "UID verification failure"},
			         {FailureOperation::kRegainSucceeds,
			          {"clear capability sets", "read capability sets", "getresgid", "getresuid",
			           "query fsuid", "query fsgid", "query supplementary groups",
			           "root-regain probe", "fatal"},
			          "successful root regain"}}) {
				FakePrivilegeContext context;
				context.failure     = failure;
				const auto result   = drop(context);
				auto       expected = through_setresuid;
				expected.insert(expected.end(), suffix.begin(), suffix.end());
				ok &= verify_fatal_result(context, result, expected, label);
			}

			for (const auto &[gid_mismatch, uid_mismatch, suffix, label] :
			     std::vector<std::tuple<bool, bool, std::vector<std::string>, std::string>>{
			         {true, false, {"getresgid", "fatal"}, "GID mismatch"},
			         {false, true, {"getresgid", "getresuid", "fatal"}, "UID mismatch"}}) {
				FakePrivilegeContext context;
				context.gid_mismatch = gid_mismatch;
				context.uid_mismatch = uid_mismatch;
				const auto result    = drop(context);
				auto       expected  = through_setresuid;
				expected.insert(expected.end(), {"clear capability sets", "read capability sets"});
				expected.insert(expected.end(), suffix.begin(), suffix.end());
				ok &= verify_fatal_result(context, result, expected, label);
			}

			auto through_identity_verification = through_setresuid;
			through_identity_verification.insert(
			    through_identity_verification.end(),
			    {"clear capability sets", "read capability sets", "getresgid", "getresuid"});

			for (const auto &[fsuid_mismatch, fsgid_mismatch, suffix, label] :
			     std::vector<std::tuple<bool, bool, std::vector<std::string>, std::string>>{
			         {true, false, {"query fsuid", "fatal"}, "post-drop fsuid mismatch"},
			         {false,
			          true,
			          {"query fsuid", "query fsgid", "fatal"},
			          "post-drop fsgid mismatch"}}) {
				FakePrivilegeContext context;
				context.fsuid_mismatch = fsuid_mismatch;
				context.fsgid_mismatch = fsgid_mismatch;
				const auto result      = drop(context);
				auto       expected    = through_identity_verification;
				expected.insert(expected.end(), suffix.begin(), suffix.end());
				ok &= verify_fatal_result(context, result, expected, label);
				ok &= expect(context.set_uids == std::array<uid_t, 3>{context.target_uid,
				                                                      context.target_uid,
				                                                      context.target_uid},
				             label + " occurs after setresuid completes");
			}

			FakePrivilegeContext group_query_context;
			group_query_context.failure   = FailureOperation::kGetgroups;
			const auto group_query_result = drop(group_query_context);
			auto       group_query_events = through_identity_verification;
			group_query_events.insert(
			    group_query_events.end(),
			    {"query fsuid", "query fsgid", "query supplementary groups", "fatal"});
			ok &= verify_fatal_result(group_query_context, group_query_result, group_query_events,
			                          "post-drop supplementary-group query failure");

			FakePrivilegeContext residual_group_context;
			residual_group_context.supplementary_groups                        = {2000};
			residual_group_context.retain_supplementary_groups_after_setgroups = true;
			const auto residual_group_result = drop(residual_group_context);
			ok &= verify_fatal_result(residual_group_context, residual_group_result,
			                          group_query_events, "post-drop residual supplementary group");
			return ok;
		}

	}  // namespace

	inline auto run_compare_privileges_privileged_tests_impl() -> bool {
		bool ok = true;
		ok &= test_privileged_success();
		ok &= test_privileged_residual_capabilities();
		ok &= test_lookup_and_identity_failures();
		ok &= test_privileged_pre_mutation_failures();
		ok &= test_privileged_failure_paths();
		return ok;
	}

}  // namespace howdy::test::compare_privileges

auto run_compare_privileges_privileged_tests() -> bool {
	return howdy::test::compare_privileges::run_compare_privileges_privileged_tests_impl();
}
