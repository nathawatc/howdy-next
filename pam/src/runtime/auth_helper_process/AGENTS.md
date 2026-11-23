# Auth Helper Process Guidelines

Parent: [`../AGENTS.md`](../AGENTS.md). Related:
[`../../../../howdy/src/auth_helper/AGENTS.md`](../../../../howdy/src/auth_helper/AGENTS.md).

## Boundaries

- `internal.hpp` (`HelperSpawn`) is private cross-TU plumbing, not a public or test seam.
- Keep `auth_helper_process` intact as one auditable lifecycle rather than fragmenting it for LOC.

## Invariants

- Receive strictly one `protocol/runtime_descriptor_message.hpp` message followed by EOF
  (`ReceiveHelperResult`); fail closed on missing message, extra message, malformed payload/control,
  or deadline expiry.
- Reap and log the helper's exit status only — never let exit status decide the open result (host
  auto-reap changes nothing).
- Point helper stdio at `/dev/null`; diagnostics go to syslog.
- Hold the spawn-time pidfd in `HelperSpawn`. If taking a pidfd fails, fail the spawn unless the
  error proves the helper was already reaped elsewhere (`ESRCH`, `ECHILD`, or `EINVAL`; see
  `PidfdErrorMeansChildGone`), in which case the socket message alone decides.
