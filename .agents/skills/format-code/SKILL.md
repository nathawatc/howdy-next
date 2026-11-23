---
name: format-code
description: Format and lint touched files. Use after editing C/C++, CMake, Markdown, or shell scripts, or when checking formatting and style.
---

# Format and Lint Code

Run only the branches matching the file types changed in your working tree. Check touched files
first with `git status --short`.

## Steps

1. **C/C++ (`*.cpp`, `*.hpp`)** — format in place using `.clang-format`:

   ```sh
   # Touched files (preferred):
   clang-format -i <files...>

   # Full repository sweep:
   git ls-files '*.cpp' '*.hpp' | xargs clang-format -i
   ```

   Verify formatting by inspecting `git diff` (or comparing `clang-format` stdout against the file).
   Use `-i` rather than `--dry-run` or `--dry-run --Werror`, because LLVM 22 can emit false-positive
   no-op replacements under `SeparateDefinitionBlocks: Always` with `UseTab: ForIndentation` (see
   `.clang-format`).

2. **CMake (`*CMakeLists.txt`, `*.cmake`)** — format with `neocmakelsp` and enforce the 100-column
   limit (`C0301`, which `format` does not check):

   ```sh
   git ls-files -z '*CMakeLists.txt' '*.cmake' | xargs -0 sh -c 'neocmakelsp format -i "$@" && awk '\''length > 100 { print FILENAME ":" FNR ": " length; bad = 1 } END { exit bad }'\'' "$@"' sh
   ```

   For a focused edit, pass only the touched CMake files to the same `sh -c` snippet.

3. **Markdown (`*.md`)** — format and lint against `.rumdl.toml` and `.editorconfig`:

   ```sh
   rumdl fmt .
   rumdl check .
   ```

4. **Shell (`*.sh`)** — lint with `shellcheck` and format with `shfmt` (reads `.editorconfig`):

   ```sh
   git ls-files '*.sh' | xargs shellcheck
   shfmt -w .
   ```

## Done Criteria

- Every touched C/C++, CMake, Markdown, and shell file is formatted cleanly.
- CMake column check reports zero lines over 100 columns.
- `rumdl check` and `shellcheck` exit 0 with zero diagnostics.
- `git diff` contains only intended formatting and code changes.
