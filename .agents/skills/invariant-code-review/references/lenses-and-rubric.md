# Review Lenses, Validation Rubric, and Output Format

Reference companion to [`SKILL.md`](../SKILL.md). Load during review to brief parallel `research`
subagents (or execute sequentially in single-pass mode) and filter/cluster findings.

## Subagent Briefing Rules

Include these constraints in every `research` subagent prompt:

- Strictly read-only inspection (files, diffs, `git log`/`git blame`); never run builds, tests,
  formatters, or `clang-tidy`. Call tools only when needed for a specific check.
- Receive: exact diff command/range, concise intent summary, and the list of scoped `AGENTS.md`
  files.
- Return candidate findings grouped by root invariant with exact `path:line` citations, concrete
  failure trigger, and boundary-level fix—never cosmetic or wording suggestions.

## The Three Specialized Lenses

### Lens 1: `AGENTS.md` & Boundary Compliance

Prompt focus:

1. Read root `AGENTS.md` and every scoped nested `AGENTS.md` for the modified files.
2. Check every changed file strictly against rules scoped to its directory ancestry (and explicitly
   linked `Related:` seams).
3. Specifically audit for:
   - Violated **parse-once boundary** rules (raw `std::filesystem::path`, unowned raw `int` fds, or
     unvalidated strings passed across internal helpers; repeated `lexically_normal()` or
     `parent_path()` calls; implicit precondition comments like *"safe now that X ran"*).
   - Unaudited sibling entry points sharing the modified boundary.
   - Missing table-driven negative tests covering the full malformed input/failure class when
     security or boundary logic changed.
   - Violations of target ownership, dependency isolation, or fail-closed contracts.
4. Quote the exact `AGENTS.md` file and rule text for every compliance finding.

### Lens 2: Bug, Defensive & Security Audit

Prompt focus:

1. Inspect the diff and surrounding functions/callers for **every** genuine correctness, defensive,
   or security defect—including subtle low-probability edge cases:
   - Memory safety, lifetime, UB, integer overflow/truncation, uninitialized state, or dangling
     views/references.
   - Descriptor leaks, missing `O_CLOEXEC`/`O_NOFOLLOW`, TOCTOU/symlink/path-traversal races,
     `parent_path()` / trailing-slash / `/.` / `..` / embedded-`\0` quirks, ownership/mode bypasses.
   - Privilege-drop ordering, seccomp/rlimit/pidfd/reaping bugs, partial-failure rollback gaps,
     staged-file cleanup leaks, or mishandled `kDurabilityUncertain` / uncertain-commit states.
   - Missing defensive checks, unchecked error returns, subtle state-transition bugs, or error
     messages that actively mislead callers or omit required diagnostic context.
2. Flag every input- or state-dependent bug where you can construct a concrete failing input,
   state transition, or race sequence against the changed boundary—never skip a real bug because
   it seems unlikely.
3. Do **not** flag pre-existing untouched issues, cosmetic/wording preferences, or Settled Contracts.

### Lens 3: Git History & Regression Context

Prompt focus:

1. Run targeted `git log -n 10 --oneline -- <modified files>` and `git blame` on modified regions
   to uncover prior invariants, security fixes, and documented limitations.
2. Inspect sibling callers and tests of modified helpers to verify:
   - No caller's assumptions were silently broken or shifted onto another layer.
   - No prior bug fix, fail-closed branch, or regression test was weakened or bypassed.
   - No masked bug in a caller or sibling entry point was exposed by the change.
   - Any inline *"known limitation"* / *"won't fix"* comment at the owning boundary is identified
     so Lens 1 and Lens 2 treat it as a Settled Contract.

## Validation & Confidence Scoring Rubric

Validate every candidate finding against the live source files before reporting. Every validated
genuine defect (score >= 80) is **Blocking**—there is no severity drop-off for real bugs:

| Score | Meaning | Action |
| --- | --- | --- |
| 0–25 | False positive, misread code, pre-existing untouched issue, cosmetic/wording churn, or Settled Contract. | Drop. |
| 50–75 | Speculative concern with no proven trigger, or stylistic/cosmetic preference with zero behavior/invariant impact. | Drop. |
| 80–90 | Verified correctness bug, subtle edge case, missing defensive check, error/rollback gap, or scoped `AGENTS.md` invariant violation with concrete trigger. | **Blocking** (cluster under root invariant). |
| 100 | Certain bug/UB/security bypass or direct contract break proven from code and rules. | **Blocking** (cluster under root invariant). |

### Mandatory Exclusions (Anti-Cosmetic Churn & Settled Contracts)

Drop immediately (score 0) when a candidate is any of the following:

- **Zero-behavior-change wording/doc churn:** Comment rewording, documentation edits, or error
  message phrasing tweaks—unless the message actively misleads callers or omits required diagnostic
  context.
- **Zero-behavior-change naming/style churn:** Renaming variables/helpers or stylistic refactors
  that do not fix an invariant, ownership boundary, or concrete readability hazard.
- **Settled Contracts (documented boundary limitations):** Explicitly documented inline next to the
  owning boundary or in `AGENTS.md` (e.g., kernel semantics like ptrace/signals/credentials/job
  control, `DisableCompareTracing` best-effort `/proc/self/task` check, `abort_if_ssh` won't-fix)
  unless the diff alters that owning boundary.
- **Pre-existing untouched defect:** Exists solely in unmodified code outside the diff's causal
  blast radius.
- **Formatter/linter-only noise:** Caught automatically by `clang-format`, `rumdl`, or `clang-tidy`,
  or explicitly silenced with a justified `NOLINT` comment.
- **Unscoped guideline:** Cited from an `AGENTS.md` belonging to an unrelated subsystem.

## Clustered Review Report Format

```markdown
## Review Summary (Round <N> — <Full Review | Convergence Re-Review>)

- **Verdict:** `APPROVED TO COMMIT` | `CHANGES REQUIRED` | `ESCALATE`
- **Blocking Invariants:** <count>

### Blocking Invariants (Must Fix Before Commit)

#### INV-1: <Broken System Invariant or Boundary> (Confidence: <80-100>)

- **Contract / Rule:** <Exact AGENTS.md quote or correctness/defensive/security invariant>
- **Affected Sites & Siblings:**
  - `path/to/file.hpp:L190-L199` — <symptom 1>
  - `path/to/file.hpp:L254-L263` — <symptom 2>
  - Sibling entry points & callers to audit together: `<FnA>`, `<FnB>`, `<FnC>`
- **Concrete Trigger / Failure Mode:** <Exact input, state transition, or race>
- **Required Invariant Fix:** <Single boundary/type repair + class-wide table-driven negative tests>
```
