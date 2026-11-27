#include "compare/sandbox.hpp"
#include "internal.hpp"
#include "support/error_message.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

	auto SandboxResourceName(howdy::native::CompareSandboxResource resource) -> const char * {
		switch (resource) {
			case howdy::native::CompareSandboxResource::kCpu:
				return "CPU";
			case howdy::native::CompareSandboxResource::kOpenFiles:
				return "file-descriptor";
			case howdy::native::CompareSandboxResource::kCore:
				return "core-dump";
			case howdy::native::CompareSandboxResource::kAddressSpace:
				return "address-space";
			case howdy::native::CompareSandboxResource::kNone:
				return "unknown";
		}
		return "unknown";
	}

	void EmitStderrWithErrno(std::string_view message, int error_number) {
		std::cerr << message;
		if (error_number != 0) {
			std::cerr << ": " << howdy::native::SystemErrorMessage(error_number);
		}
		std::cerr << "\n";
	}

}  // namespace

namespace howdy::native::compare_entrypoint_private {

	void ReportSandboxFailure(const howdy::native::CompareSandboxResult &result) {
		switch (result.status) {
			case howdy::native::CompareSandboxStatus::kOk:
				return;
			case howdy::native::CompareSandboxStatus::kNoNewPrivilegesFailure:
				EmitStderrWithErrno("Failed to enable no_new_privs sandboxing",
				                    result.error_number);
				return;
			case howdy::native::CompareSandboxStatus::kDumpableFailure:
				EmitStderrWithErrno("Failed to disable compare process dumpability",
				                    result.error_number);
				return;
			case howdy::native::CompareSandboxStatus::kTracerInspectionFailure:
				EmitStderrWithErrno("Failed to check whether compare is being traced",
				                    result.error_number);
				return;
			case howdy::native::CompareSandboxStatus::kTracerAttached:
				std::cerr << "Compare is being traced; refusing to authenticate\n";
				return;
			case howdy::native::CompareSandboxStatus::kLimitInspectionFailure:
				EmitStderrWithErrno(std::string("Failed to inspect ") +
				                        SandboxResourceName(result.resource) + " sandbox limit",
				                    result.error_number);
				return;
			case howdy::native::CompareSandboxStatus::kLimitBelowMinimum:
				std::cerr << "Inherited " << SandboxResourceName(result.resource)
				          << " limit is below Howdy's minimum sandbox policy\n";
				return;
			case howdy::native::CompareSandboxStatus::kLimitApplicationFailure:
				EmitStderrWithErrno(std::string("Failed to apply ") +
				                        SandboxResourceName(result.resource) + " sandbox limit",
				                    result.error_number);
				return;
			case howdy::native::CompareSandboxStatus::kInvalidDependencies:
				EmitStderrWithErrno("Invalid compare sandbox dependencies", result.error_number);
				return;
		}
	}

	void ReportSeccompFailure(const howdy::native::CompareSeccompResult &result) {
		switch (result.status) {
			case howdy::native::CompareSeccompStatus::kOk:
				return;
			case howdy::native::CompareSeccompStatus::kUnsupported:
				EmitStderrWithErrno("Seccomp filter sandboxing is not supported by the kernel",
				                    result.error_number);
				return;
			case howdy::native::CompareSeccompStatus::kUnsupportedArchitecture:
				EmitStderrWithErrno("Seccomp filter sandboxing is not implemented for this build",
				                    result.error_number);
				return;
			case howdy::native::CompareSeccompStatus::kFilterConstructionFailure:
				EmitStderrWithErrno("Failed to construct compare seccomp filter",
				                    result.error_number);
				return;
			case howdy::native::CompareSeccompStatus::kInstallationFailure:
				EmitStderrWithErrno("Failed to install compare seccomp filter",
				                    result.error_number);
				return;
			case howdy::native::CompareSeccompStatus::kInvalidDependencies:
				EmitStderrWithErrno("Invalid compare seccomp dependencies", result.error_number);
				return;
		}
	}

}  // namespace howdy::native::compare_entrypoint_private
