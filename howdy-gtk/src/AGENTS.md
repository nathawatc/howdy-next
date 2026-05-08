# Howdy-Gtk/src/

GTK3 optional auth UI. Sticky authentication popup plus configuration GUI.

## WHERE TO LOOK

| File               | Purpose                                                          |
| ------------------ | ---------------------------------------------------------------- |
| `authsticky.py`    | Floating auth popup shown during PAM auth (cairo, sticky window) |
| `window.py`        | Main configuration window (user list, model list, notebook tabs) |
| `onboarding.py`    | First-run setup wizard (7 slides, downloads dlib models)         |
| `tab_video.py`     | Camera preview tab using OpenCV + PixbufLoader                   |
| `tab_models.py`    | Face model management (add user, add model, list)                |
| `i18n.py`          | gettext translation wrapper, localedir relative to src           |
| `paths_factory.py` | Path getters for config, models, logos, wireframes               |

Wireframes: `main.glade`, `onboarding.glade` (GTK builder XML)

## CONVENTIONS

- GTK3 via `gi.require_version` then `from gi.repository import Gtk as gtk`
- Builder pattern: `gtk.Builder()` + `add_from_file()` + `connect_signals()`
- Periodic tasks via `gobject.timeout_add(ms, callback)`
- Canvas rendering via `cairo.ImageSurface` + `widget.connect("draw", ...)`
- CLI calls via `subprocess.getstatusoutput(["howdy", ...])`
- Translation: `_ = translation.gettext` from `i18n` module

## ANTI-PATTERNS

- DO NOT use `_thread` - GTK main loop blocks, use `gobject.timeout_add` instead
- DO NOT call `gtk.main()` multiple times - single instance per process
- DO NOT hardcode paths - use `paths_factory` getters
- DO NOT import OpenCV at module level in tab_video.py - catch ImportError gracefully
