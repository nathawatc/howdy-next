# Storage Guidelines

Read [`../../AGENTS.md`](../../AGENTS.md) first.

## Ownership

- `include/storage/user_model_types.hpp`: user-model value and result contracts
- `include/storage/user_model_label.hpp`: model label validation contract
- `include/storage/user_model_path.hpp`: user-model file path resolution contract
- `include/storage/user_models.hpp`: public list/mutation/load operation API
- `user_model_codec.cpp`: private document lifecycle, mutability, and serialization
- `user_model_codec/parsing.cpp`: JSON grammar, limits, duplicate-key checks, raw entry decoding
- `user_model_store.cpp`: secure user-model transaction boundary
- `user_models.cpp`: high-level list/mutation/load operations
- `user_model_readiness.cpp`: readiness routing and canonical checks

Private cross-TU contracts live under the matching `src/storage/<component>/internal.hpp`. Public
`user_model_types.hpp` value/result consumers must not inherit codec, locking, transaction, path, or
filesystem-security implementation.

## Invariants

Preserve codec grammar, strict/non-strict behavior, numeric and nesting limits, duplicate direct-key
rejection, finite encodings, model-count/encoding-count bounds, IDs, compatibility checks, and
serialization behavior.

`user_model_store.cpp` owns namespace lock → secure open → snapshot/identity check → mutate/remove
→ staged commit/exchange → rollback/cleanup → durability. Keep this transaction cohesive; do not
split it for LOC.

A model path is only ever absent or a committed model. Writers never create a placeholder: an update
exchanges the staged file with the model (`RENAME_EXCHANGE`), and a first model is linked in with
`RENAME_NOREPLACE`, which reports a path filled since the open as `kModelChanged`. A crash leaves at
most a staged temp file, which the next write removes. An empty model file is therefore invalid
storage everywhere.

Writers lock only `<models_dir>.lock` (`LockModelNamespace`), a root-only `0600` file beside the
models directory, and check it is a regular, root-owned, single-link file that no group or other
user can open. Never lock the models directory or a model file: `flock` needs only a readable
descriptor, a `0755` models directory is readable by every user, and the auth helper hands each
user a read-only descriptor to their own model. A lock a user can hold would stall root's writers.

`LoadUserModelsFromFd` loads from a descriptor another process opened, with the same rules as
`LoadRuntimeConfigFromFd`: no path walk, an explicit owner, open read-only and not `O_PATH`, one
link, and a positional read.

`user_model_readiness` remains descriptor-relative and fail closed. Keep
`storage/user_model_store/test_hooks.hpp` in production includes because the production store
implements those deterministic hooks.
