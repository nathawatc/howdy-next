---
name: invariant-code-review
description: Read-only code review for local diffs, branches, or PRs and anti-piecemeal resolution of review findings. Use for code review, pre-commit security/invariant audits, or re-review during a review-fix cycle until zero genuine defects remain.
---

# Invariant Code Review

Strictly read-only code review and invariant-first fix workflow for security-critical systems
codebases. Catches every genuine correctness, defensive, edge-case, and invariant defect—no matter
how subtle—while eliminating cosmetic/wording churn, redundant build/linter runs, and piecemeal
patching.

- Consult [`references/lenses-and-rubric.md`](references/lenses-and-rubric.md) for the three review
  lens prompts, 0–100 validation rubric, anti-cosmetic/settled-contract filters, and report format.
- Consult [`references/review-fix-protocol.md`](references/review-fix-protocol.md) whenever resolving
  blocking findings or running Round 2+ convergence re-review.

## Reviewer Boundary: Strictly Read-Only

The reviewer inspects diffs, source files, tests, `AGENTS.md`, and git history only. **Do not** run
CMake builds, CTest suites, `clang-tidy`, or formatters during review—the implementer/fixer already
runs verification after editing.

## Scope & Mode Selection

1. **Identify the diff under review:**
   - Default (local): uncommitted tracked changes (`git diff HEAD`) plus any untracked files
     (`git status --short` / `git ls-files --others --exclude-standard`), or branch changes
     (`git diff <base>...HEAD`).
   - PR mode (when requested): `gh pr diff <PR>` and `gh pr view <PR>`.
   - Skip review if there are no tracked or untracked source/config changes (or only generated
     output).
2. **Collect scoped `AGENTS.md` rules:**
   - Gather root `AGENTS.md` plus every nested `AGENTS.md` along each modified file's directory
     ancestry (and any `Related:` guide crossing a touched seam). Apply only rules scoped to the
     modified files.
3. **Choose execution mode:**
   - **Parallel subagents (default for multi-file or security-critical diffs):** Launch 3 read-only
     `research` subagents (`invoke_subagent`) in parallel—one per lens from
     [`references/lenses-and-rubric.md`](references/lenses-and-rubric.md)—passing the diff scope,
     intent summary, and scoped `AGENTS.md` paths.
   - **Structured single-pass (small/focused diffs or when subagents are unavailable):** Run the
     same 3 lenses sequentially in one context without skipping validation or clustering.

## Round 1: Full Multi-Lens Review

1. **Run the 3 review lenses** (prompts in
   [`references/lenses-and-rubric.md`](references/lenses-and-rubric.md)):
   - **Lens 1 — `AGENTS.md` & Boundary Compliance:** Audit modified code against scoped `AGENTS.md`
     rules, parse-once boundaries, typed handles (`ScopedFd`), and table-driven negative test
     requirements.
   - **Lens 2 — Bug, Defensive & Security Audit:** Audit the diff and surrounding call sites for
     every genuine functional, defensive, or security defect—memory/UB, lifetime/fd leaks,
     TOCTOU/symlink/path hazards, ownership/mode checks, missing error or rollback paths, and
     subtle low-probability input or state edge cases.
   - **Lens 3 — Git History & Regression Context:** Inspect `git log`/`git blame` and sibling
     callers for broken invariants, shifted assumptions, masked bugs, or re-introduced regressions.
2. **Validate & filter findings (confidence >= 80):**
   - Independently verify every candidate finding against live source files (using a parallel
     validation subagent per candidate on large reviews, or direct file verification).
   - Score 0–100 per [`references/lenses-and-rubric.md`](references/lenses-and-rubric.md). Drop
     unverified/speculative candidates (< 80), pre-existing untouched issues, linter/formatter-only
     noise, **zero-behavior-change cosmetic/wording churn**, and **Settled Contracts**.
3. **Cluster by Root Invariant (never emit flat line-by-line lists):**
   - Merge all symptoms caused by the same broken invariant or split boundary into one clustered
     finding (e.g., *"INV-1: Path parsing/normalization split across 4 helpers"* listing every site
     and sibling entry point together).
4. **Blocking Rule (No Severity Drop-Off):**
   - Treat **every validated genuine correctness bug, subtle edge case, missing defensive check,
     error/rollback gap, TOCTOU/state hazard, or scoped `AGENTS.md` invariant violation** as
     **Blocking**. Do not downgrade real defects to optional notes.
   - Exclude zero-behavior-change cosmetic suggestions (comment/doc rewording, non-misleading error
     phrasing tweaks, non-hazard renames/stylistic refactors) and documented boundary limitations
     (**Settled Contracts**).

## Resolving Findings & Progress-Based Convergence

When resolving Blocking findings (see
[`references/review-fix-protocol.md`](references/review-fix-protocol.md)):

1. **Anti-piecemeal fix mandate:** Before editing, enumerate the full input/state/syscall failure
   matrix and audit all sibling entry points in one pass. Repair the shared boundary parser/helper
   or validated type once, update all sibling callers uniformly, and lock the entire malformed class
   with table-driven negative tests. Never stack localized 3-line symptom guards or comments like
   *"safe now that X ran earlier"*.
2. **Round 2+ Convergence Re-Review (No Artificial Round Cap):**
   - Inspect the latest fix diff **plus** affected callers, sibling entry points, and tests so
     masked bugs or contract mismatches exposed by the fix are caught.
   - Continue review-fix rounds until **0 genuine correctness/invariant findings remain**.
   - Keep cosmetic/wording churn and Settled Contracts strictly excluded on every round.
3. **Stall / Flip-Flop Escalation:**
   - Trigger `ESCALATE` only if the reviewer and fixer flip-flop or stall on the **same invariant
     across 2 consecutive rounds** without net progress. Report the exact stuck invariant,
     conflicting constraints, and concrete options for the user.

## Exit Criterion & Done Criteria

- **Exit Criterion:** **0 Blocking findings = `APPROVED TO COMMIT`.**
- Reviewer remained strictly read-only (no builds, tests, formatters, or `clang-tidy` invoked).
- Every scoped `AGENTS.md` file along the modified paths was checked and quoted for any compliance
  finding.
- Every finding is a validated genuine functional, defensive, or invariant defect (confidence >= 80)
  clustered by root invariant, with zero cosmetic/wording churn and Settled Contracts excluded.
- Fixes followed the anti-piecemeal protocol with class-wide table-driven negative tests, and the
  loop converged to `APPROVED TO COMMIT` (or `ESCALATE` on a 2-round stall/flip-flop).
