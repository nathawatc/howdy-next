# Howdy Package Guidelines

Parent: [`../AGENTS.md`](../AGENTS.md).

## Subsystems

- [`src/app/AGENTS.md`](src/app/AGENTS.md): CLI dispatch, command catalog, completion
- [`src/auth_helper/AGENTS.md`](src/auth_helper/AGENTS.md): setuid helper that opens runtime
  descriptors
- [`src/cli/AGENTS.md`](src/cli/AGENTS.md): user-facing subcommands
- [`src/compare/AGENTS.md`](src/compare/AGENTS.md): compare process, privilege drop, sandbox
- [`src/config/AGENTS.md`](src/config/AGENTS.md): config schema, loader, secure writes
- [`src/storage/AGENTS.md`](src/storage/AGENTS.md): user-model codec, fd loader, and store
- [`src/vision/AGENTS.md`](src/vision/AGENTS.md): camera capture, OpenCV 5 DNN pipeline, preview
- [`tests/AGENTS.md`](tests/AGENTS.md): native test layout, fuzzers, and fixtures

## Package Invariants

- `howdy` is the sole installed user-facing CLI; install privileged helpers (`howdy-compare`, setuid
  `howdy-auth-helper`) under `<libexecdir>/howdy`.
- Keep shared face/capture policy under `include/support/` so config, storage, compare, and CLI
  never depend on vision for shared contracts.
- Reuse `support/fd_io.hpp`, `support/atomic_files.hpp`, and `support/file_security.hpp` for bounded
  I/O, staged files, and secure paths.
- Pass paths unnormalized into `file_security.hpp` public entry points (`*DirectoryTree`,
  `*FileParent`, `*WithDirectory`) so the boundary rejects relative, NUL, and `..` paths. Inside
  `support/file_security.hpp` and `support/file_security/`, parse and normalize the target and
  `ValidationRoot` once in one boundary parser into a validated target type; internal helpers
  consume that validated target instead of raw `std::filesystem::path` and never repeat
  `lexically_normal()`, `parent_path()`, or target-gate checks.
- Pass owned descriptors as `ScopedFd` across internal `howdy` helpers (reserve borrowed raw `int`
  fds for thin single-syscall wrappers such as `fd_io.hpp`, `CheckSecureFd`, and `CheckReadOnlyFd`).
  When changing path-security logic, verify every sibling `file_security.hpp` entry point against a
  shared table-driven negative matrix (`""`, `"."`, `".."`, `"/"`, `"//"`, trailing `/` or `/.`,
  embedded `\0`, symlink root/ancestor/leaf, and out-of-root targets).
- Run shared model readiness/integrity checks (`model_assets/model_file.hpp`) before inference or
  authentication.
- Expose OpenCV headers as system includes through `howdy_opencv`.
- Cross-process wire formats live in `include/protocol/` (`runtime_descriptor_message.hpp`,
  `compare_descriptors.hpp`, `compare_verdict.hpp`); use their shared constants/codec and reject any
  deviation.
- `howdy/CMakeLists.txt` and `howdy/cmake/tests/*.cmake` own target membership: keep each
  translation unit in one intended target and keep `howdy/src` off global include paths. The
  exceptions rebuild units in another configuration: fuzzers, for coverage, and
  `howdy_compare_sandbox_unsupported_arch_test`.
- `howdy/cmake/docs.cmake` (`howdy_docs`) generates `howdy.1`, `howdy.ini.5`, and `pam_howdy.8`
  from `howdy_command_catalog`, `howdy_pam_option_catalog`, `howdy_config_schema`, and `*.in`
  templates; update catalogs, schema, or templates rather than generated output.
