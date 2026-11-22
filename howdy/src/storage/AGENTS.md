# Storage Guidelines

Read [`../../AGENTS.md`](../../AGENTS.md) first.

## Ownership

- `include/storage/user_model_types.hpp`: user-model value and result contracts
- `include/storage/user_model_status.hpp`: fine-grained `UserModelStatus` codes, coarse
  `UserModelOutcome`, and `constexpr Classify(UserModelStatus)`
- `include/storage/user_model_label.hpp`: model label validation contract
- `include/storage/user_model_path.hpp`: user-model file path resolution contract
- `include/storage/user_model_store.hpp`: public `UserModelStore` value API (runtime descriptor
  open, list, inspect, load, append, verified remove, and snapshot-verified clear)
- `include/storage/user_models.hpp`: free-function `LoadUserModelsFromFd` contract
- `user_model_codec.cpp`: private document lifecycle, mutability, and serialization
- `user_model_codec/parsing.cpp`: JSON grammar, limits, duplicate-key checks, raw entry decoding
- `user_model_store.cpp`: `Production()` (environment path overrides stay out of PAM), thin public
  list/inspect/load/mutate operations and document loading
  (`LoadDocumentFromFd`, `FlattenLoadedModels`)
- `user_model_store/internal.hpp`: private store contracts (document loading, `Mutate`,
  `InspectRegularFileStatus`) for `user_model_store.cpp`, `user_models.cpp`, `transaction.cpp`, and
  `mutations.cpp`
- `user_model_store/readiness/internal.hpp`: private readiness contracts implemented by
  `readiness.cpp`; no codec or writer contracts
- `user_model_store/readiness.cpp`: `UserModelStore` constructor, the read-only path walk,
  fail-closed readiness walk, runtime descriptor open, and shared `ValidateOpenedModelFd`. It builds
  as `howdy_user_model_readiness`, the only storage library root PAM links (through
  `howdy_runtime_descriptors`); keep the codec, writers, and test-hook consumers out of it.
- `user_model_store/transaction.cpp`: cohesive `Mutate` transaction boundary (namespace lock, secure
  open, recheck, staged commit/exchange, move-aside removal, rollback, durability)
- `user_model_store/mutations.cpp`: append, verified remove, and snapshot-verified clear domain rules
- `user_models.cpp`: descriptor-only `LoadUserModelsFromFd` entrypoint

Private cross-TU contracts live under the matching `src/storage/<component>/internal.hpp`. Public
`user_model_types.hpp` value/result consumers must not inherit codec, locking, transaction, path, or
filesystem-security implementation.

## Invariants

Preserve codec grammar, strict/non-strict behavior, numeric and nesting limits, duplicate direct-key
rejection, finite encodings, model-count/encoding-count bounds, IDs, compatibility checks, and
serialization behavior.

`UserModelStore` is configured at construction with `{models_dir, owner_uid, validation_root,
hooks}`. Owner UID is always explicit (0 in production via `UserModelStore::Production()`,
`geteuid()` in tests); never reintroduce `DefaultSecureOwnerUid()` or a skip mode in the store path.

`user_model_store/transaction.cpp` owns namespace lock → secure open → revalidation/identity check →
decide(document or snapshot) → staged commit/exchange or move-aside removal → rollback/cleanup →
durability inside one `Mutate` step. Keep this transaction cohesive; do not split it for LOC.

Writers classify path-walk failures as readers do (`CheckReadiness`): a policy violation is
`kInsecurePath`, and a walk that fails with an errno (EACCES, EIO) is an inspection failure,
`kParseError`.

A model path is only ever absent or a committed model. Writers never create a placeholder, and
every change a writer notices at the path after the open is reported as `kModelChanged`. A crash
leaves at most a staged temp file, which the next write removes, and an empty model file is invalid
storage. The exchange, no-replace link, and move-aside steps are commented in
`user_model_store/transaction.cpp`. Go through `AtomicRename` in `support/atomic_files.hpp`; do not
call `renameat2` here. Treat a commit-sync failure (`kDurabilityUncertain`) as potentially
committed.

Writers lock only `.howdy-writers.lock`, a root-only `0600` file in the models directory
(`LockModelNamespace`). Never lock a model file: the auth helper gives each user a read-only
descriptor to their own model, which is enough to stall root's writers. Never lock the models
directory either: its safety depends on its `0750` mode, not on this code.

`LoadUserModelsFromFd` loads from a descriptor another process opened, with the same rules as
`LoadRuntimeConfigFromFd`: no path walk, an explicit owner, open read-only and not `O_PATH`, one
link, non-empty, and a positional read. `ValidateOpenedModelFd` backs `OpenRuntimeDescriptor`, every
path load and the append/remove documents (through `LoadDocumentFromFd`), and
`LoadUserModelsFromFd`. Writers check the model they open with `CheckSecurePathStat` in
`LockNamespaceAndOpenModelFile`, and clear loads no document, so add any new file rule to both. The
non-empty rule is for readers only: writers skip it so clear can remove a damaged empty model.

Internal `CheckReadiness` and `UserModelStore::OpenRuntimeDescriptor` remain path-based (`lstat`)
and fail closed. Keep `storage/user_model_store/test_hooks.hpp` in production includes because
`UserModelStoreConfig` accepts injected deterministic fault-injection hooks at construction
(defaulting to no-ops in production).
