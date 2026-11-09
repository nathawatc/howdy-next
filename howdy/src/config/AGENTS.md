# Config Guidelines

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
- `config_document.cpp`: the only config code that calls Rust (`howdy-config` over
  `howdy_ffi.h`); schema-checked TOML parsing and comment-preserving single-option edits

Private cross-TU contracts stay in each component's `internal.hpp`. ConfigDocument and
SetConfigValue and `howdy set` input parsing (ParseOptionValueText,
IsSafeConfigText) live in `config_document/internal.hpp`; semantic validation lives in
`config_validation/internal.hpp`; typed parsing helpers live in `runtime_config_loader/internal.hpp`.
Config tests include these private contracts directly. TOML syntax, escaping, and layout belong to
`crates/howdy-config`; never parse or emit TOML text in C++. The one exception is the packaged
config's fixed header comment in `config_template.cpp`, which Rust parses with every edit.

## Invariants

- `config_schema` is the single source of truth for option metadata and packaged fallback values.
  Generate packaged `config.toml`; do not hand-edit generated defaults.
- Runtime consumers use typed config; do not add raw `ConfigDocument` lookups outside config
  parsing/loading code.
- Consumers that only need `RuntimeConfig` link to `howdy_runtime_config`, not
  `howdy_runtime_config_loader`; secure loading, filesystem, UID, TOML parsing, and validation remain
  behind the loader and private config implementation.
- Keep secure path checks, locks, revalidation, staged install, parent sync, and commit-state
  handling intact.
- Preserve expected-current-content stale detection.
- Keep TOML text editing separate from filesystem transaction policy.
- Unknown tables/keys and wrong TOML types are structural errors, not ignored values.
- Treat post-rename sync failure as potentially committed.

Tests under `tests/config/` cover parsing, path security, atomic write/replace, runtime loading, and
update behavior. Do not weaken failure-path coverage when simplifying helpers.
