#include "common/compare_exit.hpp"
#include "common/compare_privileges_internal.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <pwd.h>
#include <string>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <vector>

#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <linux/capability.h>
#include <linux/securebits.h>

namespace {

	using howdy::native::CompareExit;
	using howdy::native::ComparePrivilegeStatus;
	using howdy::native::compare_privileges_internal::ComparePrivilegeDependencies;

	int   marker_fd              = -1;
	gid_t group_pointer_sentinel = 0;

	enum class LookupMode {
		kSuccess,
		kMissing,
		kError,
		kErange,
	};

	enum class FailureOperation {
		kNone,
		kSecurebits,
		kAmbient,
		kGroups,
		kSetresgid,
		kSetresuid,
		kCapset,
		kCapget,
		kInitialCapget,
		kFinalCapget,
		kGetgroups,
		kInitialGetresgid,
		kInitialGetresuid,
		kGetresgid,
		kGetresuid,
		kRegainSucceeds,
	};

	enum class NonzeroCapability {
		kEffective,
		kPermitted,
		kInheritable,
	};

	constexpr std::array<std::pair<NonzeroCapability, const char *>, 3> kCapabilityCases{{
	    {NonzeroCapability::kEffective, "effective"},
	    {NonzeroCapability::kPermitted, "permitted"},
	    {NonzeroCapability::kInheritable, "inheritable"},
	}};

	struct FakePrivilegeContext {
		std::array<uid_t, 3> uids{};
		std::array<gid_t, 3> gids{};
		uid_t                target_uid = 65534;
		gid_t                target_gid = 65534;

		LookupMode       lookup_mode = LookupMode::kSuccess;
		FailureOperation failure     = FailureOperation::kNone;

		int                securebits                                  = 0;
		bool               gid_mismatch                                = false;
		bool               uid_mismatch                                = false;
		bool               fsuid_mismatch                              = false;
		bool               fsgid_mismatch                              = false;
		bool               retain_supplementary_groups_after_setgroups = false;
		uid_t              fsuid                                       = 0;
		gid_t              fsgid                                       = 0;
		std::vector<gid_t> supplementary_groups;
		std::array<__user_cap_data_struct, _LINUX_CAPABILITY_U32S_3> capabilities{};
		std::array<__user_cap_data_struct, _LINUX_CAPABILITY_U32S_3> capabilities_after_capset{};
		std::vector<std::string>                                     events;

		std::string lookup_name;
		std::size_t largest_lookup_buffer = 0;
		int         lookup_calls          = 0;
		int         getgroups_calls       = 0;
		int         getresgid_calls       = 0;
		int         getresuid_calls       = 0;
		int         capget_calls          = 0;

		std::size_t          group_count   = 1;
		const gid_t         *group_pointer = &group_pointer_sentinel;
		std::array<gid_t, 3> set_gids{};
		std::array<uid_t, 3> set_uids{};
		uid_t                regain_uid = 1;

		bool capset_header_valid = false;
		bool capset_data_zero    = false;
		bool capget_header_valid = false;

		int         fatal_calls     = 0;
		int         fatal_exit_code = -1;
		std::string fatal_message;
	};

	auto expect(bool condition, const std::string &message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << "\n";
			return false;
		}
		return true;
	}

	auto expect_events(const FakePrivilegeContext     &context,
	                   const std::vector<std::string> &expected, const std::string &message)
	    -> bool {
		return expect(context.events == expected, message);
	}

	auto fake_getpwnam_r(void *raw_context, const char *name, passwd *pwd,
	                     [[maybe_unused]] char *buffer, std::size_t buffer_size, passwd **result)
	    -> int {
		auto &context = *static_cast<FakePrivilegeContext *>(raw_context);
		context.events.emplace_back("lookup");
		context.lookup_name = name;
		context.lookup_calls++;
		context.largest_lookup_buffer = std::max(context.largest_lookup_buffer, buffer_size);

		switch (context.lookup_mode) {
			case LookupMode::kSuccess:
				pwd->pw_uid = context.target_uid;
				pwd->pw_gid = context.target_gid;
				*result     = pwd;
				return 0;
			case LookupMode::kMissing:
				*result = nullptr;
				return 0;
			case LookupMode::kError:
				*result = nullptr;
				return EIO;
			case LookupMode::kErange:
				*result = nullptr;
				return ERANGE;
		}
		return EIO;
	}

	auto fake_prctl(void *raw_context, int operation, unsigned long argument2,
	                unsigned long argument3, unsigned long argument4, unsigned long argument5)
	    -> int {
		auto &context = *static_cast<FakePrivilegeContext *>(raw_context);
		if (operation == PR_GET_SECUREBITS) {
			context.events.emplace_back("query securebits");
			if (argument2 != 0 || argument3 != 0 || argument4 != 0 || argument5 != 0) {
				return -1;
			}
			return context.failure == FailureOperation::kSecurebits ? -1 : context.securebits;
		}
		context.events.emplace_back("clear ambient capabilities");
		if (operation != PR_CAP_AMBIENT || argument2 != PR_CAP_AMBIENT_CLEAR_ALL) {
			return -1;
		}
		return context.failure == FailureOperation::kAmbient ? -1 : 0;
	}

	auto fake_getgroups(void *raw_context, int size, gid_t *groups) -> int {
		auto &context = *static_cast<FakePrivilegeContext *>(raw_context);
		context.events.emplace_back("query supplementary groups");
		context.getgroups_calls++;
		if (context.failure == FailureOperation::kGetgroups || size != 0 || groups != nullptr) {
			return -1;
		}
		return static_cast<int>(context.supplementary_groups.size());
	}

	auto fake_setgroups(void *raw_context, std::size_t count, const gid_t *groups) -> int {
		auto &context = *static_cast<FakePrivilegeContext *>(raw_context);
		context.events.emplace_back("clear supplementary groups");
		context.group_count   = count;
		context.group_pointer = groups;
		if (context.failure == FailureOperation::kGroups) {
			return -1;
		}
		if (!context.retain_supplementary_groups_after_setgroups) {
			context.supplementary_groups.clear();
		}
		return 0;
	}

	auto fake_setresgid(void *raw_context, gid_t real, gid_t effective, gid_t saved) -> int {
		auto &context = *static_cast<FakePrivilegeContext *>(raw_context);
		context.events.emplace_back("setresgid");
		context.set_gids = {real, effective, saved};
		if (context.failure == FailureOperation::kSetresgid) {
			return -1;
		}
		context.gids  = context.set_gids;
		context.fsgid = context.fsgid_mismatch ? static_cast<gid_t>(effective + 1) : effective;
		return 0;
	}

	auto fake_setresuid(void *raw_context, uid_t real, uid_t effective, uid_t saved) -> int {
		auto &context = *static_cast<FakePrivilegeContext *>(raw_context);
		context.events.emplace_back("setresuid");
		context.set_uids = {real, effective, saved};
		if (context.failure == FailureOperation::kSetresuid) {
			return -1;
		}
		context.uids  = context.set_uids;
		context.fsuid = context.fsuid_mismatch ? static_cast<uid_t>(effective + 1) : effective;
		return 0;
	}

	auto valid_capability_header(const __user_cap_header_struct &header) -> bool {
		return header.version == _LINUX_CAPABILITY_VERSION_3 && header.pid == 0;
	}

	auto fake_capset(void *raw_context, const __user_cap_header_struct *header,
	                 const __user_cap_data_struct *data) -> int {
		auto &context = *static_cast<FakePrivilegeContext *>(raw_context);
		context.events.emplace_back("clear capability sets");
		context.capset_header_valid = header != nullptr && valid_capability_header(*header);
		context.capset_data_zero    = data != nullptr;
		for (std::size_t index = 0;
		     data != nullptr && index < static_cast<std::size_t>(_LINUX_CAPABILITY_U32S_3);
		     index++) {
			context.capset_data_zero &= data[index].effective == 0 && data[index].permitted == 0 &&
			                            data[index].inheritable == 0;
		}
		if (context.failure == FailureOperation::kCapset) {
			return -1;
		}
		context.capabilities = context.capabilities_after_capset;
		return 0;
	}

	auto fake_capget(void *raw_context, __user_cap_header_struct *header,
	                 __user_cap_data_struct *data) -> int {
		auto &context = *static_cast<FakePrivilegeContext *>(raw_context);
		context.events.emplace_back("read capability sets");
		context.capget_calls++;
		context.capget_header_valid = header != nullptr && valid_capability_header(*header);
		if (context.failure == FailureOperation::kCapget ||
		    (context.failure == FailureOperation::kInitialCapget && context.capget_calls == 1) ||
		    (context.failure == FailureOperation::kFinalCapget && context.capget_calls > 1)) {
			return -1;
		}
		if (data == nullptr) {
			return -1;
		}
		std::ranges::copy(context.capabilities, data);
		return 0;
	}

	auto fake_getresgid(void *raw_context, gid_t *real, gid_t *effective, gid_t *saved) -> int {
		auto &context = *static_cast<FakePrivilegeContext *>(raw_context);
		context.events.emplace_back("getresgid");
		context.getresgid_calls++;
		if (context.failure == FailureOperation::kInitialGetresgid ||
		    (context.failure == FailureOperation::kGetresgid && context.getresgid_calls > 1)) {
			return -1;
		}
		*real      = context.gid_mismatch && context.getresgid_calls > 1
		                 ? static_cast<gid_t>(context.gids[0] + 1)
		                 : context.gids[0];
		*effective = context.gids[1];
		*saved     = context.gids[2];
		return 0;
	}

	auto fake_getresuid(void *raw_context, uid_t *real, uid_t *effective, uid_t *saved) -> int {
		auto &context = *static_cast<FakePrivilegeContext *>(raw_context);
		context.events.emplace_back("getresuid");
		context.getresuid_calls++;
		if (context.failure == FailureOperation::kInitialGetresuid ||
		    (context.failure == FailureOperation::kGetresuid && context.getresuid_calls > 1)) {
			return -1;
		}
		*real      = context.uid_mismatch && context.getresuid_calls > 1
		                 ? static_cast<uid_t>(context.uids[0] + 1)
		                 : context.uids[0];
		*effective = context.uids[1];
		*saved     = context.uids[2];
		return 0;
	}

	auto fake_query_fsuid(void *raw_context) -> uid_t {
		auto &context = *static_cast<FakePrivilegeContext *>(raw_context);
		context.events.emplace_back("query fsuid");
		return context.fsuid;
	}

	auto fake_query_fsgid(void *raw_context) -> gid_t {
		auto &context = *static_cast<FakePrivilegeContext *>(raw_context);
		context.events.emplace_back("query fsgid");
		return context.fsgid;
	}

	auto fake_setuid(void *raw_context, uid_t uid) -> int {
		auto &context = *static_cast<FakePrivilegeContext *>(raw_context);
		context.events.emplace_back("root-regain probe");
		context.regain_uid = uid;
		return context.failure == FailureOperation::kRegainSucceeds ? 0 : -1;
	}

	void fake_fatal_exit(void *raw_context, const char *message, std::size_t message_size,
	                     int exit_code) {
		auto &context = *static_cast<FakePrivilegeContext *>(raw_context);
		context.events.emplace_back("fatal");
		context.fatal_calls++;
		context.fatal_exit_code = exit_code;
		context.fatal_message.assign(message, message_size);
	}

	auto make_dependencies(FakePrivilegeContext &context) -> ComparePrivilegeDependencies {
		return {
		    .context       = &context,
		    .getpwnam_r    = fake_getpwnam_r,
		    .prctl         = fake_prctl,
		    .getgroups     = fake_getgroups,
		    .setgroups     = fake_setgroups,
		    .setresgid     = fake_setresgid,
		    .setresuid     = fake_setresuid,
		    .capset        = fake_capset,
		    .capget        = fake_capget,
		    .getresgid     = fake_getresgid,
		    .getresuid     = fake_getresuid,
		    .query_fsuid   = fake_query_fsuid,
		    .query_fsgid   = fake_query_fsgid,
		    .regain_setuid = fake_setuid,
		    .fatal_exit    = fake_fatal_exit,
		};
	}

	auto drop(FakePrivilegeContext &context) -> howdy::native::ComparePrivilegeResult {
		return howdy::native::compare_privileges_internal::drop_compare_privileges(
		    make_dependencies(context));
	}

	auto expected_privileged_events() -> std::vector<std::string> {
		return {"getresuid",
		        "getresgid",
		        "lookup",
		        "query securebits",
		        "clear ambient capabilities",
		        "clear supplementary groups",
		        "setresgid",
		        "setresuid",
		        "clear capability sets",
		        "read capability sets",
		        "getresgid",
		        "getresuid",
		        "query fsuid",
		        "query fsgid",
		        "query supplementary groups",
		        "root-regain probe"};
	}

	void set_non_root_identity(FakePrivilegeContext &context) {
		context.uids  = {1000, 1000, 1000};
		context.gids  = {1000, 1000, 1000};
		context.fsuid = 1000;
		context.fsgid = 1000;
	}

	void set_capability(std::array<__user_cap_data_struct, _LINUX_CAPABILITY_U32S_3> &capabilities,
	                    NonzeroCapability                                             capability) {
		switch (capability) {
			case NonzeroCapability::kEffective:
				capabilities[0].effective = 1;
				break;
			case NonzeroCapability::kPermitted:
				capabilities[0].permitted = 1;
				break;
			case NonzeroCapability::kInheritable:
				capabilities[0].inheritable = 1;
				break;
		}
	}

	auto verify_fatal_result(const FakePrivilegeContext                  &context,
	                         const howdy::native::ComparePrivilegeResult &result,
	                         const std::vector<std::string>              &expected_events,
	                         const std::string                           &label) -> bool;

	auto expected_non_root_events() -> std::vector<std::string> {
		return {"getresuid",
		        "getresgid",
		        "query fsuid",
		        "query fsgid",
		        "read capability sets",
		        "clear ambient capabilities",
		        "clear capability sets",
		        "read capability sets",
		        "root-regain probe"};
	}

	auto test_non_root() -> bool {
		bool ok = true;

		FakePrivilegeContext context;
		set_non_root_identity(context);
		const auto result = drop(context);
		ok &= expect(result.ok(), "matching non-root credentials succeed");
		ok &= expect_events(context, expected_non_root_events(), "non-root sequence is exact");
		ok &= expect(context.lookup_calls == 0, "non-root process does not resolve nobody");
		ok &= expect(context.capset_header_valid && context.capset_data_zero,
		             "non-root capset clears all capability sets");
		ok &= expect(context.capget_header_valid, "non-root capget verifies capability sets");
		ok &= expect(context.regain_uid == 0, "non-root root-regain probe requests UID zero");

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
			const auto invalid_result = drop(invalid_context);
			ok &= verify_fatal_result(invalid_context, invalid_result,
			                          {"getresuid", "getresgid", "fatal"}, label);
		}

		for (const auto &[groups, label] : std::vector<std::pair<std::vector<gid_t>, std::string>>{
		         {{}, "zero supplementary groups"},
		         {{44}, "one supplementary group"},
		         {{10, 44, 998}, "multiple supplementary groups"},
		         {{1000}, "effective GID duplicated in supplementary groups"}}) {
			FakePrivilegeContext group_context;
			set_non_root_identity(group_context);
			group_context.supplementary_groups = groups;
			const auto group_result            = drop(group_context);
			ok &= expect(group_result.ok(), label + " are preserved");
			ok &= expect_events(group_context, expected_non_root_events(),
			                    label + " preserve non-root verification order");
			ok &= expect(group_context.group_count == 1 &&
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
		         {1000, 0, {"getresuid", "getresgid", "query fsuid", "query fsgid"}, "root fsgid"},
		         {1000,
		          1001,
		          {"getresuid", "getresgid", "query fsuid", "query fsgid"},
		          "mismatched nonzero fsgid"}}) {
			FakePrivilegeContext filesystem_context;
			set_non_root_identity(filesystem_context);
			filesystem_context.fsuid     = fsuid;
			filesystem_context.fsgid     = fsgid;
			const auto filesystem_result = drop(filesystem_context);
			auto       expected          = events;
			expected.emplace_back("fatal");
			ok &= verify_fatal_result(filesystem_context, filesystem_result, expected, label);
		}

		for (const auto &[capability, label] : kCapabilityCases) {
			FakePrivilegeContext capability_context;
			set_non_root_identity(capability_context);
			set_capability(capability_context.capabilities, capability);
			const auto capability_result = drop(capability_context);
			ok &= verify_fatal_result(capability_context, capability_result,
			                          {"getresuid", "getresgid", "query fsuid", "query fsgid",
			                           "read capability sets", "fatal"},
			                          std::string("initial non-root ") + label + " capability");
		}

		for (const auto &[capability, label] : kCapabilityCases) {
			FakePrivilegeContext residual_context;
			set_non_root_identity(residual_context);
			set_capability(residual_context.capabilities_after_capset, capability);
			const auto residual_result = drop(residual_context);
			ok &= verify_fatal_result(residual_context, residual_result,
			                          {"getresuid", "getresgid", "query fsuid", "query fsgid",
			                           "read capability sets", "clear ambient capabilities",
			                           "clear capability sets", "read capability sets", "fatal"},
			                          std::string("residual ") + label + " capability");
		}

		for (const auto &[failure, events, label] :
		     std::vector<std::tuple<FailureOperation, std::vector<std::string>, std::string>>{
		         {FailureOperation::kInitialCapget,
		          {"getresuid", "getresgid", "query fsuid", "query fsgid", "read capability sets",
		           "fatal"},
		          "initial capability inspection failure"},
		         {FailureOperation::kFinalCapget,
		          {"getresuid", "getresgid", "query fsuid", "query fsgid", "read capability sets",
		           "clear ambient capabilities", "clear capability sets", "read capability sets",
		           "fatal"},
		          "final capability inspection failure"}}) {
			FakePrivilegeContext capability_context;
			set_non_root_identity(capability_context);
			capability_context.failure   = failure;
			const auto capability_result = drop(capability_context);
			ok &= verify_fatal_result(capability_context, capability_result, events, label);
		}

		for (const auto &[failure, suffix, label] :
		     std::vector<std::tuple<FailureOperation, std::vector<std::string>, std::string>>{
		         {FailureOperation::kAmbient, {"fatal"}, "ambient-capability clear failure"},
		         {FailureOperation::kCapset,
		          {"clear capability sets", "fatal"},
		          "capability-clear failure"},
		         {FailureOperation::kRegainSucceeds,
		          {"clear capability sets", "read capability sets", "root-regain probe", "fatal"},
		          "successful non-root root regain"}}) {
			FakePrivilegeContext failure_context;
			set_non_root_identity(failure_context);
			failure_context.failure   = failure;
			const auto failure_result = drop(failure_context);
			auto       expected       = std::vector<std::string>{"getresuid",
			                                                     "getresgid",
			                                                     "query fsuid",
			                                                     "query fsgid",
			                                                     "read capability sets",
			                                                     "clear ambient capabilities"};
			expected.insert(expected.end(), suffix.begin(), suffix.end());
			ok &= verify_fatal_result(failure_context, failure_result, expected, label);
		}
		return ok;
	}

	auto test_initial_credential_inspection_failures() -> bool {
		bool ok = true;
		for (const auto &[failure, events, label] :
		     std::vector<std::tuple<FailureOperation, std::vector<std::string>, std::string>>{
		         {FailureOperation::kInitialGetresuid, {"getresuid"}, "initial UID inspection"},
		         {FailureOperation::kInitialGetresgid,
		          {"getresuid", "getresgid"},
		          "initial GID inspection"}}) {
			FakePrivilegeContext context;
			set_non_root_identity(context);
			context.failure     = failure;
			const auto result   = drop(context);
			auto       expected = events;
			expected.emplace_back("fatal");
			ok &= verify_fatal_result(context, result, expected, label);
			ok &= expect(context.lookup_calls == 0, label + " does not resolve nobody");
		}
		return ok;
	}

	auto test_privileged_success() -> bool {
		FakePrivilegeContext context;
		const auto           result = drop(context);
		bool                 ok     = true;
		ok &= expect(result.ok(), "privileged credential drop succeeds");
		ok &= expect_events(context, expected_privileged_events(), "privileged sequence is exact");
		ok &= expect(context.lookup_name == "nobody", "lookup resolves exactly nobody");
		ok &= expect(context.group_count == 0 && context.group_pointer == nullptr,
		             "setgroups receives zero and null pointer");
		ok &=
		    expect(context.set_gids == std::array<gid_t, 3>{context.target_gid, context.target_gid,
		                                                    context.target_gid},
		           "setresgid receives resolved GID three times");
		ok &=
		    expect(context.set_uids == std::array<uid_t, 3>{context.target_uid, context.target_uid,
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
			ok &= verify_fatal_result(context, result,
			                          {"getresuid", "getresgid", "lookup", "query securebits",
			                           "clear ambient capabilities", "clear supplementary groups",
			                           "setresgid", "setresuid", "clear capability sets",
			                           "read capability sets", "fatal"},
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
		ok &= expect(erange_context.largest_lookup_buffer == 64 * 1024,
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
		         {{SECBIT_KEEP_CAPS, "KEEP_CAPS"}, {SECBIT_NO_SETUID_FIXUP, "NO_SETUID_FIXUP"}}}) {
			FakePrivilegeContext context;
			context.securebits = securebits;
			const auto result  = drop(context);
			ok &= expect(result.status == ComparePrivilegeStatus::kCapabilityFailure,
			             std::string(label) + " is rejected");
			ok &= expect_events(context, {"getresuid", "getresgid", "lookup", "query securebits"},
			                    std::string(label) + " stops before credential mutation");
		}
		return ok;
	}

	auto verify_fatal_result(const FakePrivilegeContext                  &context,
	                         const howdy::native::ComparePrivilegeResult &result,
	                         const std::vector<std::string>              &expected_events,
	                         const std::string                           &label) -> bool {
		bool ok = true;
		ok &= expect(!result.ok(), label + " never reports success when fatal callback returns");
		ok &= expect(result.status == ComparePrivilegeStatus::kVerificationFailure,
		             label + " returns verification failure after malformed fatal callback");
		ok &= expect(context.fatal_calls == 1, label + " invokes fatal exactly once");
		ok &= expect(context.fatal_exit_code == static_cast<int>(CompareExit::kAbort),
		             label + " uses abort exit code");
		ok &= expect(context.fatal_message ==
		                 howdy::native::compare_privileges_internal::kFatalDiagnostic,
		             label + " passes fixed diagnostic");
		ok &= expect_events(context, expected_events, label + " performs no later operation");
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
		         {FailureOperation::kCapset, {"clear capability sets", "fatal"}, "capset failure"},
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
		           "query fsuid", "query fsgid", "query supplementary groups", "root-regain probe",
		           "fatal"},
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
		ok &= verify_fatal_result(residual_group_context, residual_group_result, group_query_events,
		                          "post-drop residual supplementary group");
		return ok;
	}

	void write_marker() {
		if (marker_fd >= 0) {
			const char    marker  = 'a';
			const ssize_t written = write(marker_fd, &marker, 1);
			(void)written;
		}
	}

	struct DestructorMarker {
		~DestructorMarker() {
			if (marker_fd >= 0) {
				const char    marker  = 'd';
				const ssize_t written = write(marker_fd, &marker, 1);
				(void)written;
			}
		}
	};

	auto test_production_non_root_identity_fatal_uses_exit() -> bool {
		std::array<int, 2> pipe_fds{};
		if (pipe(pipe_fds.data()) != 0) {
			return expect(false, "fatal regression pipe creation succeeds");
		}

		const pid_t child = fork();
		if (child < 0) {
			close(pipe_fds[0]);
			close(pipe_fds[1]);
			return expect(false, "fatal regression fork succeeds");
		}
		if (child == 0) {
			close(pipe_fds[0]);
			marker_fd = pipe_fds[1];
			(void)std::atexit(write_marker);
			DestructorMarker marker;
			const int        null_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
			if (null_fd >= 0) {
				(void)dup2(null_fd, STDERR_FILENO);
				close(null_fd);
			}

			FakePrivilegeContext context;
			set_non_root_identity(context);
			context.uids[2]   = 0;
			auto dependencies = make_dependencies(context);
			dependencies.fatal_exit =
			    howdy::native::compare_privileges_internal::fatal_compare_privilege_failure;
			(void)howdy::native::compare_privileges_internal::drop_compare_privileges(dependencies);
			_exit(99);
		}

		close(pipe_fds[1]);
		int status = 0;
		while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
		}
		std::array<char, 8> markers{};
		ssize_t             marker_count = 0;
		while (true) {
			const ssize_t count = read(pipe_fds[0], markers.data(), markers.size());
			if (count < 0 && errno == EINTR) {
				continue;
			}
			marker_count = count;
			break;
		}
		close(pipe_fds[0]);

		bool ok = true;
		ok &= expect(WIFEXITED(status), "fatal child exits normally through _exit");
		ok &= expect(WEXITSTATUS(status) == static_cast<int>(CompareExit::kAbort),
		             "fatal child exits with CompareExit::kAbort");
		ok &= expect(marker_count == 0, "fatal _exit skips destructor and atexit handlers");
		return ok;
	}

}  // namespace

int main() {
	bool ok = true;
	ok &= test_non_root();
	ok &= test_initial_credential_inspection_failures();
	ok &= test_privileged_success();
	ok &= test_privileged_residual_capabilities();
	ok &= test_lookup_and_identity_failures();
	ok &= test_privileged_pre_mutation_failures();
	ok &= test_privileged_failure_paths();
	ok &= test_production_non_root_identity_fatal_uses_exit();
	return ok ? 0 : 1;
}
