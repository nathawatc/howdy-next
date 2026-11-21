# Auth Helper Process Guidelines

Read [`../AGENTS.md`](../AGENTS.md) first, which in turn leads to the PAM package and repository
guides.

## Ownership

- `../auth_helper_process.cpp`: helper orchestration and the descriptor-message exchange
- `spawn.cpp`: the helper's socket pair, stdio, and spawn with its pidfd
- `io.cpp`: timeout, wait, terminate, reap
- `internal.hpp`: private cross-TU plumbing (`HelperSpawn`); not a public or test seam

## Invariants

- The helper's whole result is one message on its socket
  (`protocol/runtime_descriptor_message.hpp`): the marker and model state, plus `SCM_RIGHTS`
  carrying the config and, when the model is ready, the model. `ReceiveHelperResult` requires
  exactly that message and then end of file. No message, a second message, a malformed one, or the
  deadline fails the open.
- The helper's exit status is reaped and only logged. It never decides the result, so a host that
  reaps the helper itself changes nothing.
- Its stdio is `/dev/null`, and its diagnostics go to syslog.
- Its pidfd is taken with the child and held in `HelperSpawn`. When it cannot be taken, the spawn
  fails, unless the helper was already reaped elsewhere (`ESRCH`, `ECHILD`, or `EINVAL`; see
  `PidfdErrorMeansChildGone`). Then nothing is left to name or stop, and the message already in
  the socket decides.

Do not fragment `auth_helper_process` further for LOC; its process lifecycle is intentionally one
auditable subsystem.
