---
name: verify-changes
description: Build, test, and run incremental clang-tidy locally. Use when compiling C++/CMake changes, running CTest suites, or running clang-tidy.
---

# Verify Changes Locally

Run the branches matching your change. Iterate with focused tests, then finish with full preset
verification and incremental `clang-tidy`.

## Steps

1. **Focused iteration** — configure a preset, build the affected target, and run matching CTest
   suites (check `howdy/cmake/tests/*.cmake` or `pam/cmake/tests/*.cmake` when a suite spans
   multiple TUs):

   ```sh
   cmake --preset debug
   cmake --build --preset debug --target <target> --parallel "$(nproc)"
   ctest --preset debug -R <regex>
   ```

2. **Full preset build and test** — run the full relevant preset before closing a change; run both
   `release` and `debug` for broad or cross-cutting changes:

   ```sh
   # Release
   cmake --preset release
   cmake --build --preset release --parallel "$(nproc)"
   ctest --preset release

   # Debug
   cmake --preset debug
   cmake --build --preset debug --parallel "$(nproc)"
   ctest --preset debug
   ```

3. **Incremental `clang-tidy`** — run whenever C/C++ files under `howdy/` or `pam/` changed
   (matches `.github/workflows/clang_tidy.yml`):

   ```sh
   cmake --preset debug
   cmake --build --preset debug --target howdy_version_metadata
   cmake -DBUILD_DIR=build/debug -DBASE="$(git merge-base HEAD main)" \
     -P cmake/tidy_incremental.cmake
   ```

   Omit `-DBASE=...` to check only uncommitted edits against `HEAD`. Always pass
   `BUILD_DIR=build/debug`, never `build/release` (`_FORTIFY_SOURCE=3` triggers false positives; see
   `cmake/tidy_incremental.cmake`).

## Done Criteria

- Every affected target builds without errors (and without warnings with
  `-DHOWDY_WARNINGS_AS_ERRORS=ON`).
- Every required CTest preset passes with 0 failures.
- `cmake/tidy_incremental.cmake` against `build/debug` exits 0 with zero diagnostics on changed
  translation units.
