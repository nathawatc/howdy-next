# Storage Guidelines

Read [`../../AGENTS.md`](../../AGENTS.md) first.

## Ownership

- `include/storage/user_model_types.hpp`: user-model value and result contracts
- `include/storage/user_model_codec.hpp`: binary model codec, exported limits, and label rules
- `include/storage/user_model_path.hpp`: the `<user>.model` file name and path resolution
- `include/storage/user_models.hpp`: public list/mutation/load operation API
- `user_model_codec.cpp`: the only C++ caller of the Rust `howdy-user-model` codec; maps codec
  errors to `UserModelStatus` and messages
- `user_model_store.cpp`: secure user-model transaction boundary
- `user_models.cpp`: high-level list/mutation/load operations
- `user_model_readiness.cpp`: readiness routing and canonical checks
- `user_model_readiness/staged.cpp`: staged-runtime ACL/path/model validation

Private cross-TU contracts live under the matching `src/storage/<component>/internal.hpp`. Public
`user_model_types.hpp` value/result consumers must not inherit codec, locking, transaction, path, or
filesystem-security implementation.

## Invariants

The byte layout, CRC32C, limits, id order, finite-value checks, and label rules are owned by the
Rust `howdy-user-model` crate (see [`../../../crates/AGENTS.md`](../../../crates/AGENTS.md)); do not
re-implement or re-check them in C++. Decoding has one strict mode for every caller, and encoding
is canonical. Backend/model compatibility is policy and lives only in `user_models.cpp`.

`user_model_store.cpp` owns secure open → lock → snapshot/identity check → mutate/remove → staged
commit/exchange → rollback/cleanup → durability. Keep this transaction cohesive; do not split it for
LOC.

`user_model_readiness` remains descriptor-relative and fail closed. Keep
`storage/user_model_store/test_hooks.hpp` in production includes because the production store
implements those deterministic hooks.
