# Set the disable flag

from __future__ import annotations

import builtins
import sys

import paths_factory
from config_utils import load_config, update_config_value
from i18n import _

# Get the absolute filepath
config_path = paths_factory.config_file_path()

# Read config from disk
config = load_config()

# Check if enough arguments have been passed
if not getattr(builtins, "howdy_args").arguments:
    print(_("Please add a 0 (enable) or a 1 (disable) as an argument"))
    sys.exit(1)

# Get the cli argument
argument: str = getattr(builtins, "howdy_args").arguments[0]

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
if out_value == config.get("core", "disabled", fallback="true"):
    print(_("The disable option has already been set to ") + out_value)
    sys.exit(1)

try:
    update_config_value(config_path, "disabled", out_value, lock=True)
except KeyError:
    print(_('Could not find a "{}" config option to set').format("disabled"))
    sys.exit(1)

# Print what we just did
if out_value == "true":
    print(_("Howdy has been disabled"))
else:
    print(_("Howdy has been enabled"))
