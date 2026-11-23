---
name: update-translations
description: Update and validate PAM gettext catalogs. Use when editing translatable PAM strings, POTFILES, LINGUAS, or .po files.
---

# Update PAM Translations

Keep `pam/po/POTFILES`, `pam/po/LINGUAS`, and `pam/po/*.po` aligned whenever translatable PAM
messages change (`Translate(...)` calls).

## Steps

1. **Sync catalog lists** — if you added or moved a source file calling `Translate(...)`, update
   `pam/po/POTFILES`. If you added a locale, list it in `pam/po/LINGUAS` and create
   `pam/po/<locale>.po`.
2. **Extract template and merge `.po` catalogs** (configure `release` first if `build/release` does
   not exist):

   ```sh
   cmake --preset release
   cmake --build --preset release --target howdy-pot --parallel "$(nproc)"
   cmake --build --preset release --target howdy-update-po --parallel "$(nproc)"
   ```

3. **Edit translations** — update new or fuzzy entries in `pam/po/*.po`. Keep `pam/po/howdy.pot`
   untracked (it is generated and gitignored).
4. **Compile and validate catalogs**:

   ```sh
   cmake --build --preset release --target codegen --parallel "$(nproc)"
   ctest --preset release -R pam-translation-catalogs
   ```

## Done Criteria

- `codegen` builds every `.mo` catalog without `msgfmt --check` errors.
- `ctest --preset release -R pam-translation-catalogs` passes with 0 failures.
- `git status` shows only intended `POTFILES`, `LINGUAS`, and `*.po` changes (no `howdy.pot`).
