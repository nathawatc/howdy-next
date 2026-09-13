# Config Guidelines

**Updated:** 2026-09-06

Read [`../../AGENTS.md`](../../AGENTS.md) first.

## Ownership

- `config_schema.cpp`: canonical option table, lookup APIs, formatting, packaged fallbacks
- `config_schema/validation.cpp`: schema consistency validation
- `runtime_config.cpp`: typed runtime values and schema-backed defaults
- `runtime_config_loader.cpp`: secure typed runtime loading
- `runtime_config_loader/defaults.cpp`: runtime-only path/owner loading adapters
- `config_utils.cpp`: high-level secure config transaction orchestration
- `config_utils/file_ops.cpp`: locks, secure file I/O, staging/install, expected-content checks,
  content validation
- `config_utils/key_file.cpp`: INI scalar/line parsing and comment-preserving replacement

Private cross-TU contracts stay in each component's `internal.hpp`. ConfigReader and strict float
parsing live in `config_reader/internal.hpp`; semantic validation lives in
`config_validation/internal.hpp`; typed parsing helpers live in `runtime_config_loader/internal.hpp`.
Config tests include these private contracts directly; do not expose INIReader through public headers.

## Invariants

- `config_schema` is the single source of truth for option metadata and packaged fallback values.
  Generate packaged `config.ini`; do not hand-edit generated defaults.
- Runtime consumers use typed config; do not add raw `ConfigReader` lookups outside config
  parsing/loading code.
- Keep secure path checks, locks, revalidation, staged install, parent sync, and commit-state
  handling intact.
- Preserve expected-current-content stale detection.
- Keep INI text editing separate from filesystem transaction policy.
- Treat post-rename sync failure as potentially committed.

Tests under `tests/config/` cover parsing, path security, atomic write/replace, runtime loading, and
update behavior. Do not weaken failure-path coverage when simplifying helpers.
