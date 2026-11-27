---
name: agents-md-architect
description: Author, audit, or refactor root and nested AGENTS.md files. Use when creating a new AGENTS.md, pruning duplicate or stale rules across the AGENTS.md hierarchy, or hardening engineering invariants against piecemeal review patching.
---

# AGENTS.md Architect

Design and refactor `AGENTS.md` trees for security-critical systems codebases: minimal always-loaded
context, zero parent-child duplication, and enforceable pre-edit gates instead of vague advice.

Consult [`references/patterns.md`](references/patterns.md) when rewriting weak rules, stripping
anti-patterns, or scaffolding a new root, package, or subsystem `AGENTS.md`.

## Design Principles

- **Spend context load deliberately:** Every line in root `AGENTS.md` loads on every turn. Keep root
  lean, universal, and high-signal (build/test commands, layout, repo-wide invariants). Push
  subsystem-specific paths, types, and failure matrices to nested `AGENTS.md` files, and push
  multi-step recipes, examples, and templates into skills or `references/`.
- **Single source of truth (SSOT):** Each invariant lives at the highest level where it is universal
  and nowhere else. Child files open with `Parent: [...]`, inherit all ancestor rules without
  restating them, and add only local boundaries, symbols, and deltas. Leave one-lookup facts
  (directory trees, CLI flags, config files) to the environment instead of caching stale copies.
- **Co-locate by concept:** Group a subsystem's boundaries, invariants, and caveats under tight
  headings so reading one rule brings its neighbors with it.
- **Enforceable gates over aspirations:** Phrase rules as checkable, exhaustive gates using strong
  leading words (*pre-edit failure matrix*, *parse-once boundary*, *invariant fix*, *regression
  lock*, *owning boundary*). State the positive target behavior first; pair any hard ban directly
  with what to do instead.
- **Prune sediment and no-ops:** Delete prose the model already obeys by default, stale file or
  symbol references, and historical commentary that belongs inline next to the code.

## Mandatory Systems Invariants

Ensure the repository's `AGENTS.md` hierarchy enforces all four gates (universal rules in root;
concrete types and input matrices in package/subsystem files):

1. **Pre-edit failure matrix:** Before writing or fixing code, enumerate boundary inputs, state
   transitions, syscall/TOCTOU races, and rollback/error paths, and audit every sibling entry point
   sharing that boundary in one pass.
2. **Parse once at the boundary:** Parse, validate, and normalize raw primitives
   (`std::filesystem::path`, raw `int` fds, unvalidated strings) in one boundary parser/constructor
   into a validated type or owned handle (`ScopedFd`). Pass that representation across internal
   helpers, call normalization/splitting (`lexically_normal()`, `parent_path()`) in that single
   boundary function only, and reject comments claiming an earlier check made a helper safe.
3. **Invariant fix and regression lock:** On every bug or review finding, name the violated system
   invariant before editing, repair the shared boundary helper and all sibling callers uniformly
   (never stack localized symptom guards), and lock the whole malformed input or failure class with
   table-driven negative tests across all affected entry points.
4. **Boundary ownership:** Fix an edge case in code only when it closes the gap at its owning
   boundary. When closing it would force userspace to re-derive kernel semantics (ptrace,
   credentials, signals, job control) or only shift assumptions onto callers, document the known
   limitation and owning invariant inline at the owning boundary—and treat documented limitations as
   settled contracts during review unless the owning boundary changes.

## Workflow

1. **Inventory the full hierarchy:** Find and read every `AGENTS.md` in the repo
   (`find . -name "AGENTS.md" -not -path '*/.*' -not -path './build*' | sort`). Map `Parent:` /
   `Child:` / `Related:` links, duplicate rules, vague wording, and stale references.
2. **Place each rule at one tier:**
   - **Root `AGENTS.md`:** Repo map, build/test commands, global layout, universal edge-case/review
     gates, global security & commit rules.
   - **Package `AGENTS.md` (`howdy/`, `pam/`):** Subsystem map, package-wide contracts, shared
     support types (`ScopedFd`, `file_security.hpp`), concrete negative test matrices.
   - **Subsystem `AGENTS.md` (`src/<mod>/`, `tests/`, `ci/`):** Component seams, target ownership,
     local failure invariants, and pointers to inline known limitations.
3. **Edit for signal-to-noise:**
   - Remove verbatim or paraphrased repeats of ancestor rules; link siblings with `Related:` when
     crossing seams instead of copying lists.
   - Replace vague heuristics (e.g., *"while designing"*, *"when the fix is local"*) with checkable
     pre-edit and diff-verifiable gates from [`references/patterns.md`](references/patterns.md).
   - Keep line length within the repo's Markdown limit (100 columns under `.rumdl.toml`).
4. **Verify every reference and lint:**
   - Check every relative Markdown link resolves on disk.
   - Check every cited file path, CMake target, build/test command, and C++ symbol exists in the
     repository (excluding intentionally absent files explicitly marked as omitted).
   - Run `rumdl check` on every created or modified Markdown file.

## Done Criteria

- Every `AGENTS.md` in the repository has been audited against the hierarchy and its parent/child
  links resolve cleanly.
- Zero rules are duplicated between ancestor and descendant `AGENTS.md` files.
- All four Mandatory Systems Invariants are encoded as enforceable gates without vague escape
  hatches.
- Every referenced file path, build/test command, CMake target, and C++ symbol is verified against
  the repository.
- `rumdl check` exits 0 with zero diagnostics on all touched Markdown files.
