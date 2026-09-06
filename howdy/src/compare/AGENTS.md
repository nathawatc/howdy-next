# Compare Guidelines

**Updated:** 2026-09-06

Read [`../../AGENTS.md`](../../AGENTS.md) first.

## Ownership

- `capture_session.*`: camera lifecycle, timeout clock, frame statistics, exposure restore
- `engine.*`: preprocessing, validation, detection/embedding adapters, match selection
- `sandbox.*`: sandbox policy
- `processing.*`: shared processing helpers
- `src/bin/compare.cpp`: composition, arguments, config/model load, output/report policy, exit
  mapping

Privilege dropping is one security-sensitive subsystem:

- `privileges.cpp`: high-level policy/orchestration
- `privileges/operations.cpp`: syscall/account adapters
- `privileges/verification.cpp`: credential/capability transition and verification
- `privileges/internal.hpp`: private cross-TU contract
- `include/compare/privileges/internal.hpp`: separate shared/test seam

## Invariants

Preserve privilege-transition ordering, capability clearing, ID/fs-ID checks, root-regain
verification, and fatal fail-closed behavior. Do not move policy into syscall adapters or split the
privilege subsystem further for size alone.

Keep compare engine/capture tests independent from real ONNX inference when they are testing
orchestration or semantic result mapping.
