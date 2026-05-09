# Set a config value

from __future__ import annotations

import builtins
import os
import sys
import tempfile

import paths_factory
from i18n import _

# Get the absolute filepath
config_path = paths_factory.config_file_path()

howdy_args = getattr(builtins, "howdy_args")

# Check if enough arguments have been passed
if len(howdy_args.arguments) < 2:
    print(_("Please add a setting you would like to change and the value to set it to"))
    print(_("For example:"))
    print("\n\thowdy set sface_threshold 0.363\n")
    sys.exit(1)

# Get the name and value from the cli
set_name: str = howdy_args.arguments[0]
set_value: str = howdy_args.arguments[1]


# Will be filled with the correctly config line to update
def _find_and_update_config(config_path: str, set_name: str, set_value: str) -> None:
    found_line: str = ""
    found_line_index: int = -1
    lines: list[str] = []

    with open(config_path, "r") as f:
        for i, line in enumerate(f):
            lines.append(line)
            stripped = line.strip()
            if stripped.startswith(set_name + " =") or stripped.startswith(
                set_name + " "
            ):
                found_line = line
                found_line_index = i

    if not found_line:
        print(_('Could not find a "{}" config option to set').format(set_name))
        sys.exit(1)

    lines[found_line_index] = set_name + " = " + set_value + "\n"

    fd, temp_path = tempfile.mkstemp(dir=os.path.dirname(config_path), suffix=".tmp")
    try:
        with os.fdopen(fd, "w") as tmp:
            tmp.writelines(lines)
        os.replace(temp_path, config_path)
    except Exception:
        if os.path.exists(temp_path):
            os.unlink(temp_path)
        raise


_find_and_update_config(config_path, set_name, set_value)
print(_("Config option updated"))
