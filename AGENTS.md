# Repository Guidelines

**Updated:** 2026-08-15

## Scope

Root rules. Read nearest `AGENTS.md` for narrower ownership guidance.

- `ci/AGENTS.md`: CI container and workflow configuration
- `howdy/AGENTS.md`: Howdy package include, source, tests, and resources
- `howdy/src/cli/AGENTS.md`: CLI entrypoints and command integration
- `howdy/src/vision/AGENTS.md`: OpenCV DNN and camera capture layer
- `pam/AGENTS.md`: PAM module and authentication flow

## Maintenance Mode

Howdy Next is feature-complete at the v3.4.0 architecture baseline. Default
future work is limited to:

- bug fixes
- security fixes
- platform compatibility
- dependency compatibility
- packaging/build compatibility
- narrowly required maintenance

Avoid speculative refactors, architecture churn, feature expansion without an
explicit request, coverage campaigns for their own sake, splitting cohesive
files merely because of LOC, and test-framework migrations.

File size alone is not a reason to split production code. Prefer a split only
when there are genuinely independent responsibilities and an existing
interface boundary permits separation without increasing coupling.

## Build, Install, and Style

Use CMake 3.31+ with GCC or Clang and GNU Make or Ninja.

### Release

```sh
cmake --preset release
cmake --build --preset release --parallel "$(nproc)"
ctest --preset release
```

### Debug

```sh
cmake --preset debug
cmake --build --preset debug --parallel "$(nproc)"
ctest --preset debug
```

Do not hardcode a core count in repository guidance. CMake configures the
preset build directory and produces the unified `howdy` CLI, native helpers,
compare process, and PAM module.

- Install user-facing commands through `howdy`; do not add standalone command
  executables.
- Keep privileged helpers under `<libexecdir>/howdy`; do not expose them as
  normal commands. This includes `howdy-compare` and setuid
  `howdy-auth-helper`.
- CI uses `ci/Containerfile` and
  `codeberg.org/nathawat/howdy-next/ci-1:latest`.

Target C++23. Follow `.clang-format`, use snake_case for files/functions/tests,
and preserve tabs in touched C/C++ files. Keep package modules mirrored across
`include/<module>/`, `src/<module>/`, and `tests/<module>/` when applicable.
Keep resources outside `src/`. Reuse shared storage, config, readiness, model,
and runtime helpers. Runtime code loads typed `RuntimeConfig` through
`load_runtime_config()`, not raw `ConfigReader` lookups.

For C/C++ formatting:

```sh
find howdy pam -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.c' -o -name '*.h' \) -print0 | xargs -0 clang-format -i
```

CI does not run `clang-tidy`; do not use `run-clang-tidy`. For changed code,
use `clang-tidy-diff.py` only:

```sh
git diff -U0 HEAD -- howdy pam | /usr/share/clang/clang-tidy-diff.py -p1 -path build -quiet -hide-progress
```

## Testing

Tests live under `howdy/tests/` and `pam/tests/`. Add focused `*_test.cpp`
under the matching module.

A single CTest logical suite may consist of multiple test translation units.
The file owning `main()` may be only the suite driver; it is not necessarily
the complete suite. Inspect CMake source lists and sibling `*_test.cpp` files
before assuming coverage lives in one source. Headers shared across test
translation units belong under `tests/include/<module>/`; never expose test
include directories to production targets.

Representative logical suites, not an exhaustive inventory:

- Howdy add CLI: `add_cli_test.cpp` with preflight, capture, and argument
  sources.
- Howdy config CLI: `config_cli_test.cpp` with workflow and integration
  sources.
- Howdy model download: multi-source `native-download-models` suite.
- Howdy snapshot/preview: snapshot CLI/writer and preview session/renderer
  suites.
- Howdy dispatcher/completion: app tests under `howdy/tests/app/`, separate
  from CLI source ownership.
- Howdy compare engine and compare privileges: each is a multi-source suite.
- Howdy config utilities, user-model codec/storage, and auth helper: inspect
  their CMake source lists and sibling tests before editing.
- PAM auth flow, native prompt conversation, prompt coordinator, and runtime
  session: each is organized as a logical suite across focused sources.

For security-sensitive code, cover success and failure paths. Preserve file
ownership checks, config validation, typed runtime-config loading, model
readiness/integrity, PAM status mapping, runtime staging, output-protocol
validation, and exception handling.

Run all tests with `ctest --preset <preset> --output-on-failure`. For one test,
use `ctest --preset <preset> -R '^<test-name>$' --output-on-failure`.

## Security

Do not change `config.ini` format casually. Preserve atomic config rewrites,
secure path validation, ownership expectations for `/etc/howdy`, config files,
downloaded ONNX models, and user-model files. PAM authentication fails closed
on unexpected errors. Validate auth-helper output through the shared keys in
`howdy/include/protocol/auth_helper_protocol.hpp`.

## Commit / PR

Use imperative, scoped Conventional Commit subjects, such as
`fix(pam): ...`, `test(config): ...`, `build(release): ...`, or `docs: ...`.
PRs should state the problem, summarize the change, link issues when relevant,
and include test results. Add screenshots or terminal output only when they
clarify CLI, PAM prompt, or packaging behavior.
