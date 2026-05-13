from __future__ import annotations

import builtins
import json
import os
import sys
import tempfile

import paths_factory
from i18n import _

user = getattr(builtins, "howdy_user")

if not getattr(builtins, "howdy_args").arguments:
    print(_("Please add the ID of the model you want to remove as an argument"))
    print(_("For example:"))
    print("\n\thowdy remove 0\n")
    print(_("You can find the IDs by running:"))
    print("\n\thowdy list\n")
    sys.exit(1)

if not os.path.exists(paths_factory.user_models_dir_path()):
    print(_("Face models have not been initialized yet, please run:"))
    print("\n\thowdy add\n")
    sys.exit(1)

enc_file = paths_factory.user_model_path(user)

try:
    with open(enc_file) as f:
        encodings = json.load(f)
except FileNotFoundError:
    print(_("No face model known for the user {}, please run:").format(user))
    print("\n\thowdy add\n")
    sys.exit(1)

found = False

id = getattr(builtins, "howdy_args").arguments[0]

for enc in encodings:
    if str(enc["id"]) == id:
        if not getattr(builtins, "howdy_args").y:
            # Double check with the user
            print(
                _('This will remove the model called "{label}" for {user}').format(
                    label=enc["label"], user=user
                )
            )
            ans = input(_("Do you want to continue [y/N]: "))

            if ans.lower() != "y":
                print(_('\nInterpreting as a "NO", aborting'))
                sys.exit(1)

            print()

        found = True
        break

if not found:
    print(_("No model with ID {id} exists for {user}").format(id=id, user=user))
    sys.exit(1)

if len(encodings) == 1:
    os.remove(paths_factory.user_model_path(user))
    print(_("Removed last model, howdy disabled for user"))
else:
    new_encodings = []

    for enc in encodings:
        if str(enc["id"]) != id:
            new_encodings.append(enc)

    fd, tmp_path = tempfile.mkstemp(dir=os.path.dirname(enc_file), suffix=".tmp")
    try:
        with os.fdopen(fd, "w") as datafile:
            json.dump(new_encodings, datafile)
        os.replace(tmp_path, enc_file)
    except Exception:
        if os.path.exists(tmp_path):
            os.unlink(tmp_path)
        raise

    print(_("Removed model {}").format(id))
