# Rust Crate Guidelines

Read the [repository guidelines](../AGENTS.md) first.

## Scope

Covers the Cargo workspace (`Cargo.toml`, `Cargo.lock`, `deny.toml`), `crates/`, and
`cmake/howdy_rust.cmake`.

## Roles and Flow

- Domain crates (`howdy-<domain>`) are plain `rlib`s of safe Rust. They have no C ABI knowledge
  and do not depend on each other.
- `howdy-ffi` is the only crate C++ links: one `staticlib` per binary. It depends on the domain
  crates and holds every `extern "C"` export.
- Data flows one way: C++ reads and locks files, Rust works on bytes and returns bytes or plain
  data, and C++ applies policy and commits. Rust never opens files, takes locks, touches
  descriptors, reads the environment, keeps global mutable state, or calls back into C++.
- C++ reaches Rust only through a thin wrapper that calls `howdy_link_rust_ffi()`. The generated
  `howdy_ffi.h` lives in a private include root; never include it from other code.

## Safety

- The workspace denies `unsafe_code`. Domain crates add `#![forbid(unsafe_code)]`. Only
  `howdy-ffi`'s `ffi` module allows `unsafe`, and every `unsafe` block has a `// SAFETY:` comment.
- Every export is `extern "C"`, takes byte slices as `(ptr, len)`, returns `HowdyFfiStatus`, and
  writes results to out-parameters. No Rust type crosses the boundary.
- Every export body runs inside `boundary::guard`, so a panic becomes a status and never unwinds
  into C++ or a PAM client. Keep `panic = "unwind"` in both Cargo profiles.
- Memory allocated by Rust is released only by an exported Rust free function.
- Bound input sizes on the C++ side before calling Rust, and re-check them in Rust.
- `HOWDY_FFI_ABI_VERSION` stays at 1 until the first release that ships `howdy-ffi`; after that,
  bump it whenever an export signature or status value changes.

## Dependencies

- Pin every third-party crate once in `[workspace.dependencies]`; members use `.workspace = true`.
- Keep `Cargo.lock` committed. Builds always pass `--locked`; `HOWDY_CARGO_OFFLINE=ON` adds
  `--offline` for distribution builds.
- New crates must pass `cargo deny check` and be added to `THIRD_PARTY_NOTICES.md`.
- `rust-version` in the root `Cargo.toml` is the MSRV; both CI images must meet it.

## Checks

CTest runs `rust-cargo-test`, `rust-clippy` (`-D warnings`), and `rust-fmt`. Run them directly
while iterating:

```sh
cargo test --workspace --locked
cargo clippy --workspace --all-targets --locked -- -D warnings
cargo fmt --all
```

`cargo deny` needs network access and runs outside CTest:

```sh
cmake --build --preset release --target howdy_cargo_deny
```
