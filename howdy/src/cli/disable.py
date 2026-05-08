# Set the disable flag

from __future__ import annotations

import builtins
import configparser
import fcntl
import os
import shutil
import sys
import tempfile

import paths_factory
from i18n import _


def _get_disabled_key(config: configparser.ConfigParser) -> str:
    return "disabled = " + config.get("core", "disabled", fallback=True)


def _get_disabled_value(out_value: str) -> str:
    return "disabled = " + out_value


def _update_config(
    config_path: str, out_value: str, config: configparser.ConfigParser
) -> None:
    with open(config_path, "r") as f:
        fcntl.flock(f.fileno(), fcntl.LOCK_EX)
        lines: list[str] = f.readlines()
        fcntl.flock(f.fileno(), fcntl.LOCK_UN)

    disabled_key = _get_disabled_key(config)
    disabled_value = _get_disabled_value(out_value)

    new_lines: list[str] = []
    for line in lines:
        if disabled_key in line:
            new_lines.append(disabled_value + "\n")
        else:
            new_lines.append(line)

    with tempfile.NamedTemporaryFile(
        mode="w", delete=False, dir=os.path.dirname(config_path)
    ) as tmp:
        tmp.writelines(new_lines)
        tmp_path = tmp.name

    shutil.move(tmp_path, config_path)


# Get the absolute filepath
config_path = paths_factory.config_file_path()

# Read config from disk
config = configparser.ConfigParser()
config.read(config_path)

# Check if enough arguments have been passed
if not builtins.howdy_args.arguments:
    print(_("Please add a 0 (enable) or a 1 (disable) as an argument"))
    sys.exit(1)

# Get the cli argument
argument: str = builtins.howdy_args.arguments[0]

# Translate the argument to the right string
out_value: str
if argument == "1" or argument.lower() == "true":
    out_value = "true"
elif argument == "0" or argument.lower() == "false":
    out_value = "false"
else:
    # Of it's not a 0 or a 1, it's invalid
    print(_("Please only use 0 (enable) or 1 (disable) as an argument"))
    sys.exit(1)

# Don't do anything when the state is already the requested one
if out_value == config.get("core", "disabled", fallback=True):
    print(_("The disable option has already been set to ") + out_value)
    sys.exit(1)

_update_config(config_path, out_value, config)

# Print what we just did
if out_value == "true":
    print(_("Howdy has been disabled"))
else:
    print(_("Howdy has been enabled"))
