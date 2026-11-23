# Storage Guidelines

Parent: [`../../AGENTS.md`](../../AGENTS.md). Child:
[`user_model_store/AGENTS.md`](user_model_store/AGENTS.md).

## Boundaries

- Public value/result headers (`include/storage/user_model_types.hpp`, `user_model_status.hpp`,
  `user_model_label.hpp`, `user_model_path.hpp`) stay lightweight and free of codec, locking,
  transaction, or filesystem-security dependencies.
- `user_model_codec` (`user_model_codec.cpp`, `user_model_codec/parsing.cpp`, private
  `user_model_codec/internal.hpp`) is internal to `howdy_user_models`; `user_models.cpp` exposes the
  descriptor-only `LoadUserModelsFromFd` entrypoint.
- Store construction, `howdy_user_model_readiness` PAM isolation, locking, and write transactions
  live in [`user_model_store/AGENTS.md`](user_model_store/AGENTS.md).

## Codec and Descriptor-Load Invariants

- Preserve JSON grammar, strict/non-strict parsing, numeric and nesting limits, model-count and
  encoding-count bounds, duplicate direct-key rejection, finite encoding validation, IDs,
  compatibility checks, and serialization format.
- `LoadUserModelsFromFd` enforces the same descriptor contract as `LoadRuntimeConfigFromFd` (no path
  walk, explicit owner UID, read-only non-`O_PATH` regular file, `st_nlink == 1`, positional read)
  plus non-empty size (an empty model file is damaged storage).
