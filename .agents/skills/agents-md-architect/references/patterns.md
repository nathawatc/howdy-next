# AGENTS.md Patterns and Anti-Patterns

Reference companion to [`SKILL.md`](../SKILL.md). Consult when rewriting weak rules, pruning bloated
sections, or scaffolding a new `AGENTS.md`.

## Anti-Patterns to Strip Out

| Anti-pattern | Why it fails | Replacement |
| --- | --- | --- |
| *"Map edge cases while designing"* | Review rounds bypass "designing" and rush to minimal diffs. | Make it a **pre-edit gate** for both new work and every review fix, including a sibling-entry-point audit. |
| *"Fix when the fix is local"* | Encourages 3-line symptom guards scattered across callers. | Require **invariant fixes** at the owning boundary parser/helper with uniform updates across sibling callers. |
| Raw primitives across internal helpers | Splits normalization (`lexically_normal()`, `parent_path()`) and invites *"safe now that X ran"* comments. | Mandate **parse once at the boundary** into a validated type or owned handle (`ScopedFd`); ban implicit ordering comments. |
| Single-case regression tests | Produces 10-round whack-a-mole over one malformed input at a time. | Require **table-driven negative tests** locking the entire input or failure class across all affected entry points. |
| Re-litigating kernel limits in review | Loops on userspace workarounds that only move TOCTOU or credential gaps. | Require **boundary ownership**: document known limitations inline at the owning boundary and treat them as settled contracts. |
| Parent rules copied into child files | Wastes context tokens and creates drift when the rule changes. | Keep the rule once at its universal tier; open child files with `Parent: [...]` and state only local deltas. |
| Negation-only bans (*"Don't do X"*) | Activates the forbidden pattern without giving the target path. | State the positive behavior first, then pair any hard guardrail directly with the replacement. |
| Cached directory trees or long recipes | Goes stale quickly and bloats always-loaded context. | Point to the source of truth or move multi-step workflows into `.agents/skills/<name>/SKILL.md`. |

## Hierarchy Templates

### Root `AGENTS.md`

```markdown
# Repository Guidelines

Standing rules for every change. Read the nearest nested `AGENTS.md` before editing a subsystem;
local guides add detail without relaxing these invariants.

## Map

- [`pkg/AGENTS.md`](pkg/AGENTS.md): concise scope list

## Build and CMake

- Concrete toolchain floor, build/test commands, and CMake invariants.

## Code and Layout

- Naming, module layout, dependency style, inline *why* comments, doc sync rule.

## Edge Cases

- **Pre-edit failure matrix:** ...
- **Parse once at the boundary:** ...
- **Invariant fix and regression lock:** ...
- **Boundary ownership:** ...

## Security

- Repo-wide fail-closed, privilege, durability, and typed-boundary invariants.

## Commits

- Commit format and subject-length rule.
```

### Package `AGENTS.md` (`howdy/`, `pam/`)

```markdown
# <Package> Guidelines

Parent: [`../AGENTS.md`](../AGENTS.md).

## Subsystems

- [`src/<mod>/AGENTS.md`](src/<mod>/AGENTS.md): crisp scope

## Package Invariants

- Package-scoped contracts, shared support headers, owned-handle types (`ScopedFd`), single-boundary
  parsers, and concrete negative input matrices for shared security helpers.
```

### Subsystem `AGENTS.md` (`src/<mod>/`, `tests/`, `ci/`)

```markdown
# <Subsystem> Guidelines

Parent: [`../../AGENTS.md`](../../AGENTS.md). Related: [`../<peer>/AGENTS.md`](../<peer>/AGENTS.md).

## Boundaries

- Component ownership, target isolation, intentional test seams (`include/<mod>/internal.hpp`).

## Invariants

- Subsystem-specific contracts only; reference peers for shared contracts and state only the delta.
- Pointers to inline known limitations (settled contracts) when relevant.
```

## Hardened Engineering Gates (Copy-Ready)

Use these four formulations in root `AGENTS.md` `## Edge Cases`, tailoring examples to the repo:

- **Pre-edit failure matrix:** Before writing or fixing code, enumerate the complete failure space
  across boundary inputs (empty, root/separator/trailing-slash, `.`/`..`, embedded `\0`, symlink or
  non-regular leaves, numeric/size limits), state transitions, syscall/TOCTOU races, and
  rollback/error paths, and audit every sibling entry point sharing that boundary in one pass.
- **Parse once at the boundary:** Parse, validate, and normalize raw primitives
  (`std::filesystem::path`, raw `int` fds, unvalidated strings) in one boundary parser/constructor
  that yields a validated type or owned handle, and pass only that representation across internal
  helpers (keep raw primitives at outer API or thin single-syscall edges). Call normalization and
  path splitting (`lexically_normal()`, `parent_path()`) in that single boundary function only.
  Comments asserting an earlier check made a helper safe (e.g., *"safe now that X ran"*) signal a
  broken boundary—encode the invariant in the validated type instead.
- **Invariant fix and regression lock:** On every bug or review finding, identify the broken system
  invariant before editing, repair the shared boundary parser/helper and all sibling callers
  uniformly rather than stacking localized symptom guards, and lock the fix with table-driven
  negative tests covering the entire malformed input or failure class across every affected entry
  point—not just the single reported case.
- **Boundary ownership:** Fix an edge case in code only when the fix closes the gap at the boundary
  that owns the invariant. When closing it in code would require userspace to re-derive kernel
  semantics (ptrace, credentials, signals, job control) or would only shift assumptions onto another
  caller, do not add a userspace workaround: document the known limitation and owning invariant
  inline at the owning boundary, enforce the contract there, and treat documented limitations as
  settled contracts during review unless the owning boundary changes.
