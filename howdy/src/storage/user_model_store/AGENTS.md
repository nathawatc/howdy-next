# User-Model Store Guidelines

Parent: [`../AGENTS.md`](../AGENTS.md). Related:
[`../../auth_helper/AGENTS.md`](../../auth_helper/AGENTS.md).

## Boundaries

- `readiness.cpp` and `readiness/internal.hpp` build as `howdy_user_model_readiness`, the only
  storage library root PAM links (through `howdy_runtime_descriptors`). Keep
  `user_model_store/internal.hpp`, the codec, writers, and test-hook consumers out of
  `readiness.cpp`.
- Keep `include/storage/user_model_store/test_hooks.hpp` in production includes so
  `UserModelStoreConfig` accepts deterministic fault-injection hooks (no-ops in production).
- `transaction.cpp` owns the cohesive `Mutate` boundary (namespace lock -> secure open ->
  revalidation/identity check -> decide -> staged commit/exchange or move-aside removal ->
  rollback/cleanup -> durability); keep it in one auditable unit rather than splitting for LOC.
  Domain rules for append, verified remove, and snapshot-verified clear live in `mutations.cpp`.

## Invariants

- Configure `UserModelStore` at construction with `{models_dir, owner_uid, validation_root, hooks}`
  (`owner_uid = 0` in production via `UserModelStore::Production()`, `geteuid()` in tests). Keep
  owner UID explicit with no skip mode or global hook state; keep `Production()` env overrides out
  of PAM.
- Classify writer path-walk failures identically to `CheckReadiness`: security policy violations
  return `kInsecurePath`; walk errnos (`EACCES`, `EIO`) return `kParseError`.
- Keep every model path either absent or a committed model — never create a placeholder. Report any
  post-open path or lock-directory change as `kModelChanged`. Commit through `AtomicRename`
  (`support/atomic_files.hpp`) rather than raw `renameat2`, and treat `kDurabilityUncertain` as
  potentially committed.
- Lock only `.howdy-writers.lock` (`0600`, root-only) via `LockModelNamespace`. Never lock a model
  file (auth helper gives users read-only fds that can stall root writers) or the models directory
  (safety relies on `0750`).
- Apply any new file-validation rule to both `ValidateOpenedModelFd` (backing
  `OpenRuntimeDescriptor`, `LoadDocumentFromFd`, and `LoadUserModelsFromFd`) and
  `CheckSecurePathStat` in `LockNamespaceAndOpenModelFile` (used by writers, including `clear` which
  loads no document). Enforce non-empty size on readers only so `clear` can remove damaged empty
  files.
- Keep `CheckReadiness` and `OpenRuntimeDescriptor` path-based (`lstat`) and fail closed.
