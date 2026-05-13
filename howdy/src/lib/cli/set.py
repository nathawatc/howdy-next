# Set a config value

from __future__ import annotations

import builtins
import sys

import paths_factory
from config_utils import update_config_value
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

try:
    update_config_value(config_path, set_name, set_value)
except KeyError:
    print(_('Could not find a "{}" config option to set').format(set_name))
    sys.exit(1)

print(_("Config option updated"))
