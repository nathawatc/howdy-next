#pragma once

#include "compare/privileges/internal.hpp"
#include "protocol/compare_exit.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <pwd.h>
#include <string>
#include <utility>
#include <vector>

#include <sys/prctl.h>
#include <sys/types.h>

#include <linux/capability.h>
#include <linux/securebits.h>

namespace howdy::test::compare_privileges {

	using howdy::test::expect;

	using howdy::native::CompareExit;
	using howdy::native::ComparePrivilegeStatus;
	using howdy::native::compare_privileges_internal::ComparePrivilegeDependencies;
	using howdy::native::compare_privileges_internal::FatalExitRequest;
	using howdy::native::compare_privileges_internal::GroupIdOutputs;
	using howdy::native::compare_privileges_internal::UserIdOutputs;

	inline gid_t group_pointer_sentinel = 0;

	enum class LookupMode : std::uint8_t {
		kSuccess,
		kMissing,
		kError,
		kErange,
	};

	enum class FailureOperation : std::uint8_t {
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

	enum class NonzeroCapability : std::uint8_t {
		kEffective,
		kPermitted,
		kInheritable,
	};

	constexpr std::array<std::pair<NonzeroCapability, const char *>, 3> kCapabilityCases{{
	    {NonzeroCapability::kEffective, "effective"},
	    {NonzeroCapability::kPermitted, "permitted"},
	    {NonzeroCapability::kInheritable, "inheritable"},
	}};

	constexpr std::array<std::pair<NonzeroCapability, const char *>, 2>
	    kInitiallyFatalCapabilityCases{{
	        {NonzeroCapability::kEffective, "effective"},
	        {NonzeroCapability::kPermitted, "permitted"},
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
		int         capset_calls          = 0;

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

	inline auto ExpectEvents(const FakePrivilegeContext     &context,
	                         const std::vector<std::string> &expected, const std::string &message)
	    -> bool {
		return expect(context.events == expected, message);
	}

	inline auto FakeGetpwnamR(void *raw_context, const char *name, passwd *pwd,
	                          [[maybe_unused]] char *buffer, std::size_t buffer_size,
	                          passwd **result) -> int {
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

	inline auto FakePrctl(void *raw_context, int operation, unsigned long argument2,
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

	// NOLINTNEXTLINE(readability-non-const-parameter) -- POSIX callback signature.
	inline auto FakeGetgroups(void *raw_context, int size, gid_t *groups) -> int {
		auto &context = *static_cast<FakePrivilegeContext *>(raw_context);
		context.events.emplace_back("query supplementary groups");
		context.getgroups_calls++;
		if (context.failure == FailureOperation::kGetgroups || size != 0 || groups != nullptr) {
			return -1;
		}
		return static_cast<int>(context.supplementary_groups.size());
	}

	inline auto FakeSetgroups(void *raw_context, std::size_t count, const gid_t *groups) -> int {
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

	inline auto FakeSetresgid(void *raw_context, gid_t real, gid_t effective, gid_t saved) -> int {
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

	inline auto FakeSetresuid(void *raw_context, uid_t real, uid_t effective, uid_t saved) -> int {
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

	inline auto ValidCapabilityHeader(const __user_cap_header_struct &header) -> bool {
		return header.version == _LINUX_CAPABILITY_VERSION_3 && header.pid == 0;
	}

	inline auto FakeCapset(void *raw_context, const __user_cap_header_struct *header,
	                       const __user_cap_data_struct *data) -> int {
		auto &context = *static_cast<FakePrivilegeContext *>(raw_context);
		context.events.emplace_back("clear capability sets");
		context.capset_calls++;
		context.capset_header_valid = header != nullptr && ValidCapabilityHeader(*header);
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

	inline auto FakeCapget(void *raw_context, __user_cap_header_struct *header,
	                       __user_cap_data_struct *data) -> int {
		auto &context = *static_cast<FakePrivilegeContext *>(raw_context);
		context.events.emplace_back("read capability sets");
		context.capget_calls++;
		context.capget_header_valid = header != nullptr && ValidCapabilityHeader(*header);
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

	inline auto FakeGetresgid(void *raw_context, GroupIdOutputs outputs) -> int {
		auto &context = *static_cast<FakePrivilegeContext *>(raw_context);
		context.events.emplace_back("getresgid");
		context.getresgid_calls++;
		if (context.failure == FailureOperation::kInitialGetresgid ||
		    (context.failure == FailureOperation::kGetresgid && context.getresgid_calls > 1)) {
			return -1;
		}
		*outputs.real      = context.gid_mismatch && context.getresgid_calls > 1
		                         ? static_cast<gid_t>(context.gids[0] + 1)
		                         : context.gids[0];
		*outputs.effective = context.gids[1];
		*outputs.saved     = context.gids[2];
		return 0;
	}

	inline auto FakeGetresuid(void *raw_context, UserIdOutputs outputs) -> int {
		auto &context = *static_cast<FakePrivilegeContext *>(raw_context);
		context.events.emplace_back("getresuid");
		context.getresuid_calls++;
		if (context.failure == FailureOperation::kInitialGetresuid ||
		    (context.failure == FailureOperation::kGetresuid && context.getresuid_calls > 1)) {
			return -1;
		}
		*outputs.real      = context.uid_mismatch && context.getresuid_calls > 1
		                         ? static_cast<uid_t>(context.uids[0] + 1)
		                         : context.uids[0];
		*outputs.effective = context.uids[1];
		*outputs.saved     = context.uids[2];
		return 0;
	}

	inline auto FakeQueryFsuid(void *raw_context) -> uid_t {
		auto &context = *static_cast<FakePrivilegeContext *>(raw_context);
		context.events.emplace_back("query fsuid");
		return context.fsuid;
	}

	inline auto FakeQueryFsgid(void *raw_context) -> gid_t {
		auto &context = *static_cast<FakePrivilegeContext *>(raw_context);
		context.events.emplace_back("query fsgid");
		return context.fsgid;
	}

	inline auto FakeSetuid(void *raw_context, uid_t uid) -> int {
		auto &context = *static_cast<FakePrivilegeContext *>(raw_context);
		context.events.emplace_back("root-regain probe");
		context.regain_uid = uid;
		return context.failure == FailureOperation::kRegainSucceeds ? 0 : -1;
	}

	inline void FakeFatalExit(void *raw_context, FatalExitRequest request) {
		auto &context = *static_cast<FakePrivilegeContext *>(raw_context);
		context.events.emplace_back("fatal");
		context.fatal_calls++;
		context.fatal_exit_code = request.exit_code;
		context.fatal_message.assign(request.message, request.message_size);
	}

	inline auto MakeDependencies(FakePrivilegeContext &context) -> ComparePrivilegeDependencies {
		return {
		    .context       = &context,
		    .getpwnam_r    = FakeGetpwnamR,
		    .prctl         = FakePrctl,
		    .getgroups     = FakeGetgroups,
		    .setgroups     = FakeSetgroups,
		    .setresgid     = FakeSetresgid,
		    .setresuid     = FakeSetresuid,
		    .capset        = FakeCapset,
		    .capget        = FakeCapget,
		    .getresgid     = FakeGetresgid,
		    .getresuid     = FakeGetresuid,
		    .query_fsuid   = FakeQueryFsuid,
		    .query_fsgid   = FakeQueryFsgid,
		    .regain_setuid = FakeSetuid,
		    .fatal_exit    = FakeFatalExit,
		};
	}

	inline auto Drop(FakePrivilegeContext &context) -> howdy::native::ComparePrivilegeResult {
		return howdy::native::compare_privileges_internal::DropComparePrivileges(
		    MakeDependencies(context));
	}

	inline auto ExpectedPrivilegedEvents() -> std::vector<std::string> {
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

	inline void SetNonRootIdentity(FakePrivilegeContext &context) {
		context.uids  = {1000, 1000, 1000};
		context.gids  = {1000, 1000, 1000};
		context.fsuid = 1000;
		context.fsgid = 1000;
	}

	inline void
	SetCapability(std::array<__user_cap_data_struct, _LINUX_CAPABILITY_U32S_3> &capabilities,
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

	inline void SetWakeAlarmInheritable(
	    std::array<__user_cap_data_struct, _LINUX_CAPABILITY_U32S_3> &capabilities) {
		constexpr auto capability_word            = static_cast<std::size_t>(CAP_WAKE_ALARM / 32);
		constexpr auto capability_bit             = static_cast<unsigned int>(CAP_WAKE_ALARM % 32);
		capabilities[capability_word].inheritable = static_cast<__u32>(1U << capability_bit);
	}

	inline auto ExpectedNonRootEvents() -> std::vector<std::string> {
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

	inline auto VerifyFatalResult(const FakePrivilegeContext                  &context,
	                              const howdy::native::ComparePrivilegeResult &result,
	                              const std::vector<std::string>              &expected_events,
	                              const std::string                           &label) -> bool {
		bool ok = true;
		ok &= expect(!result.Ok(), label + " never reports success when fatal callback returns");
		ok &= expect(result.status == ComparePrivilegeStatus::kVerificationFailure,
		             label + " returns verification failure after malformed fatal callback");
		ok &= expect(context.fatal_calls == 1, label + " invokes fatal exactly once");
		ok &= expect(context.fatal_exit_code == static_cast<int>(CompareExit::kAbort),
		             label + " uses abort exit code");
		ok &= expect(context.fatal_message ==
		                 howdy::native::compare_privileges_internal::kFatalDiagnostic,
		             label + " passes fixed diagnostic");
		ok &= ExpectEvents(context, expected_events, label + " performs no later operation");
		return ok;
	}

}  // namespace howdy::test::compare_privileges
