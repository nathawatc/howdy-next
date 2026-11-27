# Review-Fix Execution & Convergence Re-Review Protocol

Reference companion to [`SKILL.md`](../SKILL.md). Load when resolving `Blocking` findings or
executing Round 2+ convergence re-review.

## Anti-Piecemeal Fix Protocol (Before Editing Any Code)

Never apply a localized 3-line guard at one reported line. For each Blocking `INV-<k>` cluster,
complete all four gates in one coherent pass so each round closes an entire class of bugs cleanly:

1. **Name the broken invariant:** Restate the single system invariant or boundary contract that
   failed (from `INV-<k>`), not the individual symptom lines.
2. **Enumerate the full failure matrix & audit siblings:**
   - Map all failure dimensions for that boundary before editing:
     - Inputs: `""`, `"."`, `".."`, `"/"`, `"//"`, trailing `/` or `/.`, embedded `\0`, symlink or
       non-regular root/ancestor/leaf, out-of-root targets, numeric/size bounds.
     - Lifecycle/state: repeated calls, closed/invalid handles, partial failure, rollback/cleanup,
       post-rename sync uncertainty (`kDurabilityUncertain`).
     - Concurrency/TOCTOU: concurrent unlink/replace, `EEXIST`/`ENOENT`/`ECHILD`/`ESRCH` races.
   - List every sibling entry point and caller sharing that boundary and verify all of them
     together.
3. **Repair the owning boundary once (Parse-Once):**
   - Consolidate validation, path splitting (`parent_path()`), and normalization
     (`lexically_normal()`) into one boundary parser/constructor yielding a validated type or owned
     handle (`ScopedFd`).
   - Pass only that validated type across internal helpers; delete duplicated checks and delete any
     *"safe now that X ran earlier"* comments by encoding the guarantee in the type.
   - If closing the edge case would force userspace to re-derive kernel semantics or only shift the
     gap onto callers, stop: document the known limitation and owning invariant inline at the
     owning boundary instead of adding a workaround.
4. **Lock the entire failure class with table-driven negative tests:**
   - Add or extend a table-driven test exercising the whole malformed input or failure class across
     every affected sibling entry point—never a one-off test for only the single reported case.
   - Implementer runs builds, tests, and `clang-tidy` after editing (the reviewer remains read-only).

## Round 2+ Convergence Re-Review Protocol

Continue review-fix rounds until **0 genuine correctness/invariant findings remain**, without an
artificial iteration cap:

1. **Review scope (fix + affected blast radius):**
   - Inspect the latest fix diff, updated tests, **and all callers and sibling entry points affected
     by the fix** so masked bugs or shifted assumptions exposed by the latest change are caught.
2. **Checks on every re-review round:**
   - **Resolution check:** Was every prior `INV-<k>` resolved cleanly at its owning boundary (not
     patched with scattered guards or ordering-assumption comments) with class-wide table-driven
     negative tests?
   - **Regression & exposure check:** Did the fix break a caller contract, leak a resource, weaken a
     fail-closed path, or expose a previously masked bug in a caller or sibling entry point?
   - **Same strict exclusion filter:** Keep zero-behavior-change wording/cosmetic/naming churn and
     Settled Contracts excluded on every round.
3. **Progress-based termination:**
   - **0 remaining Blocking findings** → output `APPROVED TO COMMIT` and stop.
   - **Net progress made** (prior invariant resolved, or a newly exposed genuine bug/masked defect
     discovered in affected callers/siblings) → cluster under `INV-<k>`, output `CHANGES REQUIRED`,
     and continue.
   - **Stall or flip-flop across 2 consecutive rounds on the same invariant** without net progress →
     stop immediately and output `ESCALATE` with the stuck `INV-<k>`, conflicting constraints, and
     concrete options for the user.
