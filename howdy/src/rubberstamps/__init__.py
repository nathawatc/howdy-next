import importlib.util
import os
import re
import syslog
from syslog import LOG_ERR

from i18n import _


class RubberStamp:
    """Howdy rubber stamp"""

    pass


def execute(config, opencv):
    verbose = config.getboolean("debug", "verbose_stamps", fallback=False)
    dir_path = os.path.dirname(os.path.realpath(__file__))
    installed_stamps = []

    for filename in os.listdir(dir_path):
        if not os.path.isfile(dir_path + "/" + filename):
            continue

        if filename in ["__init__.py", ".gitignore"]:
            continue

        installed_stamps.append(filename.split(".")[0])

    if verbose:
        print("Installed rubberstamps: " + ", ".join(installed_stamps))

    raw_rules = config.get("rubberstamps", "stamp_rules")
    rules = raw_rules.split("\n")

    for rule in rules:
        rule = rule.strip()

        if len(rule) <= 1:
            continue

        regex_result = re.search(
            r"^(\w+)\s+([\w\.]+)\s+([a-z]+)(.*)?$", rule, re.IGNORECASE
        )

        if not regex_result:
            print(_("Error parsing rubberstamp rule: {}").format(rule))
            continue

        type = regex_result.group(1)

        if type not in installed_stamps:
            print(_("Stamp not installed: {}").format(type))
            continue

        spec = importlib.util.spec_from_file_location(
            type, dir_path + "/" + type + ".py"
        )
        if spec is None:
            print(_("Stamp error: Could not load spec for {}").format(type))
            continue
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)

        try:
            constructor = getattr(module, type)
        except AttributeError:
            print(_("Stamp error: Class {} not found").format(type))
            continue

        try:
            instance = constructor()
        except Exception as e:
            syslog(LOG_ERR, "Rubberstamp %s failed to construct: %s" % (type, str(e)))
            print(_("Stamp error: Class {} failed to initialize").format(type))
            continue
        instance.verbose = verbose
        instance.config = config
        instance.opencv = opencv

        instance.video_capture = opencv["video_capture"]
        instance.face_detector = opencv["face_detector"]
        instance.pose_predictor = opencv["pose_predictor"]
        instance.clahe = opencv["clahe"]

        instance.options = {
            "timeout": float(re.sub("[a-zA-Z]", "", regex_result.group(2))),
            "failsafe": regex_result.group(3) != "faildeadly",
        }

        try:
            instance.declare_config()
        except Exception:
            print(_("Internal error in rubberstamp configuration declaration:"))

            import traceback

            traceback.print_exc()
            continue

        raw_options = regex_result.group(4).split()

        for option in raw_options:
            key, value = option.split("=")

            if key not in instance.options:
                print("Unknown config option for rubberstamp " + type + ": " + key)
                continue

            if value in ("True", "False"):
                instance.options[key] = value == "True"
            elif re.match(r"^\d+$", value):
                instance.options[key] = int(value)
            elif re.match(r"^\d*\.\d+$", value):
                instance.options[key] = float(value)
            else:
                instance.options[key] = value

        try:
            instance.run()
        except Exception as e:
            syslog(LOG_ERR, "Rubberstamp %s failed to run: %s", type, str(e))
            print(_("Stamp error: Class {} failed to execute").format(type))