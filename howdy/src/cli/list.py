# List all models for a user

from __future__ import annotations

import builtins
import json
import os
import sys
import time

import paths_factory
from i18n import _

user = getattr(builtins, "howdy_user")

# Check if the models file has been created yet
if not os.path.exists(paths_factory.user_models_dir_path()):
    print(_("Face models have not been initialized yet, please run:"))
    print("\n\tsudo howdy -U " + user + " add\n")
    sys.exit(1)

# Path to the models file
enc_file = paths_factory.user_model_path(user)
try:
    with open(enc_file) as f:
        encodings = json.load(f)
except FileNotFoundError:
    if not getattr(builtins, "howdy_args").plain:
        print(_("No face model known for the user {}, please run:").format(user))
        print("\n\tsudo howdy -U " + user + " add\n")
    sys.exit(1)

# Print a header if we're not in plain mode
for enc in encodings:
    print(str(enc["id"]), end="")

    if getattr(builtins, "howdy_args").plain:
        print(",", end="")
    else:
        print((4 - len(str(enc["id"]))) * " ", end="")

    print(time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(enc["time"])), end="")
    print("," if getattr(builtins, "howdy_args").plain else "  ", end="")
    print(enc["label"])

print()
