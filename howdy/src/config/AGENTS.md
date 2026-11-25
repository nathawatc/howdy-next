# Config Guidelines

Parent: [`../../AGENTS.md`](../../AGENTS.md).

## Boundaries

- `config_schema` is the single source of truth for option metadata and packaged defaults; generate
  packaged `config.ini` through `howdy_config_generator` rather than hand-editing defaults.
- Link consumers that only need typed values to `howdy_runtime_config`, not
  `howdy_runtime_config_loader`. Keep filesystem access, UID checks, INI parsing, and validation
  behind the loader and private `src/config/<component>/internal.hpp` headers; keep `INIReader` out
  of public headers.
- Keep INI text editing (`config_utils/key_file.cpp`) separate from filesystem transaction policy
  (`config_utils/file_ops.cpp`).
- Config writes and validation report typed results (`ConfigWriteStatus`, `ConfigValidationStatus` in
  `config_utils.hpp`). Decide on the status; messages are display text only.
- `include/config/test_hooks.hpp` holds the after-open/before-read race hook used by secure config
  open/read paths.

## Invariants

- `runtime_paths.cpp` honors `HOWDY_CONFIG`, `HOWDY_MODELS_DIR`, `HOWDY_USER_MODELS_DIR`, and
  `HOWDY_LOG_PATH` only while `geteuid() == getuid()` and `getegid() == getgid()` and via
  `secure_getenv()` (ignored when setuid, setgid, or `AT_SECURE`).
- Validate the config `.lock` with `fstat` before `flock`: a regular file owned by the trusted
  owner with `st_nlink == 1` and no group/other permission bits. Fail closed with a reason instead
  of repairing it. After `flock`, fail closed unless the path still names the locked file.
- Match inih line rules when changing `key_file.cpp`; the differential fuzz
  (`tests/config/config_key_file_fuzz*.cpp`) checks every update through `ConfigReader`. Match inih
  continuation rules when replacing a value, and refuse the update when a continuation
  line looks like an assignment or section header instead of erasing it.
- Refuse a value inih would not read back unchanged (surrounding whitespace, an inline comment)
  instead of storing something else than was asked for.
- Preserve secure path checks, locking, revalidation, expected-current-content stale detection,
  staged install, parent-directory sync, and potentially-committed handling on post-rename sync
  failure (`ConfigWriteStatus::kDurabilityUncertain`).
- `LoadRuntimeConfigFromFd` walks no path, requires an explicit owner UID, requires a read-only
  non-`O_PATH` regular file with `st_nlink == 1` (concurrent unlink/replace fails closed), and reads
  positionally without advancing the shared file offset.
