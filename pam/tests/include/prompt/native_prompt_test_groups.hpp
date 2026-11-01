#pragma once

#include "prompt/conversation_restore.hpp"

#include <array>
#include <string>

auto ExpectCtrlCAbortsPromptAndRestoresTerminal() -> bool;
auto ExpectDispatchRejectsInvalidState() -> bool;
auto ExpectOriginalConversationRestored() -> bool;
auto ExpectRestoreHandlesNullPam() -> bool;
auto ExpectRestoreResult(std::array<int, 3>                    pam_set_results,
                         howdy::pam::ConversationRestoreResult expected, const std::string &message)
    -> bool;
auto ExpectDispatchThrowCleanup(int throw_mode, const std::string &message) -> bool;
auto ExpectDestroyedInstalledNativeWrapperFailsClosed() -> bool;
auto ExpectNativeTerminalEligibility() -> bool;
auto ExpectNativeTerminalAliases() -> bool;
auto ClosedStdinPreservedWithTerminalStdout() -> bool;
auto ClosedStdoutPreservedWithTerminalStdin() -> bool;
auto ClosedStderrPreservedWithTerminalStdin() -> bool;
auto ClosedStdinStdoutPreservedWithTerminalStderr() -> bool;
auto ClosedAllStdioRemainClosed() -> bool;
auto TtyNormalizationFailurePreservesClosedStdin() -> bool;
auto AbortPipeCreationFailureIsTransactional() -> bool;
auto AbortPipeFirstNormalizationFailureIsTransactional() -> bool;
auto AbortPipeSecondNormalizationFailureIsTransactional() -> bool;
auto ExpectInvalidPamTtyIsUnavailable() -> bool;
auto ExpectAbortRequestUnblocksWithoutPipeWakeup() -> bool;
auto ExpectPtyHangupAbortsPrompt() -> bool;
auto ExpectPollEintrWithoutAbortDoesNotAbortPrompt() -> bool;
auto ExpectPollEintrWithAbortFailsClosed() -> bool;
auto ExpectReadEintrRetriesAndAcceptsInput() -> bool;
auto ExpectReadEintrWithAbortFailsClosed() -> bool;
auto ExpectReadZeroRetriesAndAcceptsInput() -> bool;
auto ExpectReadEagainRetriesAndAcceptsInput() -> bool;
auto ExpectReadEagainWithAbortFailsClosed() -> bool;
auto ExpectRestoreEintrRetriesAndRestores() -> bool;
auto ExpectNativeMessageStyles() -> bool;
auto ExpectNativePromptInputEdges() -> bool;
auto ExpectOversizedPromptFailsClosed() -> bool;
auto ExpectRestoreFailureFailsClosed() -> bool;
auto ExpectPromptExceptionRestoresTerminalAndRecordsRestoreFailure() -> bool;
