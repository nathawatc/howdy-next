# Compare Guidelines

Read [`../../AGENTS.md`](../../AGENTS.md) first.

## Ownership

- `capture_session.*`: camera lifecycle, timeout clock, frame statistics, exposure restore
- `engine.*`: preprocessing, validation, detection/embedding adapters, match selection
- `sandbox.*`: sandbox policy
- `processing.*`: shared processing helpers
- `entrypoint.cpp`: composition, arguments, config/model load, output/report policy, exit mapping,
  runtime-descriptor intake (`protocol/compare_descriptors.hpp`), and the verdict byte PAM reads
  when the application reaped compare (`protocol/compare_verdict.hpp`); `src/bin/compare.cpp`
  exits with `_exit` right after it
- `src/bin/compare.cpp`: thin production wrapper that pins compare trust roots to UID 0

Privilege dropping is one security-sensitive subsystem:

- `privileges.cpp`: high-level policy/orchestration
- `privileges/operations.cpp`: syscall/account adapters
- `privileges/verification.cpp`: credential/capability transition and verification
- `privileges/internal.hpp`: private cross-TU contract
- `include/compare/privileges/internal.hpp`: separate shared/test seam

## Invariants

Compare runs in path mode or descriptor mode. In descriptor mode PAM passes the config at fd 4 and
the user model at fd 5, announced by `HOWDY_COMPARE_CONFIG_FD=4` and `HOWDY_COMPARE_MODEL_FD=5`.
`TakeRuntimeDescriptors` takes them before compare opens anything, so neither number can be one of
compare's own files. Both must be announced exactly and open, the verdict socket must be announced,
and `--config` must be absent; anything else aborts. Every descriptor announced at its exact
number is compare's from then on and is closed on every path, each right after it is loaded. The
step 1 fd loaders own the type, owner, mode, and link checks; do not duplicate them here. Path
mode stays for a PAM module loaded before the upgrade.

Preserve privilege-transition ordering, capability clearing, ID/fs-ID checks, root-regain
verification, and fatal fail-closed behavior. Do not move policy into syscall adapters or split the
privilege subsystem further for size alone.

Keep compare engine/capture tests independent from real ONNX inference when they are testing
orchestration or semantic result mapping.
