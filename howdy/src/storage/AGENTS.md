# Storage Guidelines

**Updated:** 2026-09-06

Read [`../../AGENTS.md`](../../AGENTS.md) first.

## Ownership

- `include/storage/user_model_types.hpp`: user-model value and result contracts
- `include/storage/user_models.hpp`: public list/mutation/load operation API
- `user_model_codec.cpp`: private document lifecycle, mutability, and serialization
- `user_model_codec/parsing.cpp`: JSON grammar, limits, duplicate-key checks, raw entry decoding
- `user_model_store.cpp`: secure user-model transaction boundary
- `user_models.cpp`: high-level list/mutation/load operations
- `user_model_readiness.cpp`: readiness routing and canonical checks
- `user_model_readiness/staged.cpp`: staged-runtime ACL/path/model validation

Private cross-TU contracts live under the matching `src/storage/<component>/internal.hpp`.

## Invariants

Preserve codec grammar, strict/non-strict behavior, numeric and nesting limits, duplicate direct-key
rejection, finite encodings, model-count/encoding-count bounds, IDs, compatibility checks, and
serialization behavior.

`user_model_store.cpp` owns secure open → lock → snapshot/identity check → mutate/remove → staged
commit/exchange → rollback/cleanup → durability. Keep this transaction cohesive; do not split it for
LOC.

`user_model_readiness` remains descriptor-relative and fail closed. Keep
`storage/user_model_store/test_hooks.hpp` in production includes because the production store
implements those deterministic hooks.
