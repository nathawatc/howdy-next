# CLI directly called by running the howdy command

import argparse
import builtins
import getpass
import os
import pwd
import sys

lib_path = os.path.join(os.path.dirname(__file__), "lib")
if lib_path not in sys.path:
    sys.path.insert(0, lib_path)

from i18n import _

# Try to get the original username (not "root") from shell
sudo_user = os.environ.get("SUDO_USER")
doas_user = os.environ.get("DOAS_USER")
pkexec_uid = os.environ.get("PKEXEC_UID")
pkexec_user = pwd.getpwuid(int(pkexec_uid))[0] if pkexec_uid else ""
env_user = getpass.getuser()
user = next((u for u in [sudo_user, doas_user, pkexec_user, env_user] if u), "")

if user == "":
    print(_("Could not determine user, please use the --user flag"))
    sys.exit(1)

parser = argparse.ArgumentParser(
    description=_("Command line interface for Howdy face authentication."),
    formatter_class=argparse.RawDescriptionHelpFormatter,
    add_help=False,
    prog="howdy",
    usage="howdy [-U USER] [--plain] [-h] [-y] {command} [{arguments}...]".format(
        command=_("command"), arguments=_("arguments")
    ),
    epilog=_("For support please visit\nhttps://github.com/boltgolt/howdy"),
)

parser.add_argument(
    "command",
    help=_(
        "The command option to execute, can be one of the following: add, clear, config, disable, download-models, list, remove, snapshot, set, test or version."
    ),
    metavar="command",
    choices=[
        "add",
        "clear",
        "config",
        "disable",
        "download-models",
        "list",
        "remove",
        "set",
        "snapshot",
        "test",
        "version",
    ],
)

parser.add_argument(
    "arguments",
    help=_("Optional arguments for the add, disable, remove and set commands."),
    nargs="*",
)

parser.add_argument(
    "-U", "--user", default=user, help=_("Set the user account to use.")
)

parser.add_argument("-y", help=_("Skip all questions."), action="store_true")

parser.add_argument(
    "--plain", help=_("Print machine-friendly output."), action="store_true"
)

# Overwrite the default help message so we can use a uppercase S
parser.add_argument(
    "-h",
    "--help",
    action="help",
    default=argparse.SUPPRESS,
    help=_("Show this help message and exit."),
)

if len(sys.argv) < 2:
    print(_("current active user: ") + user + "\n")
    parser.print_help()
    sys.exit(0)

args = parser.parse_args()

# Save the args and user as builtins which can be accessed by the imports
setattr(builtins, "howdy_args", args)
setattr(builtins, "howdy_user", args.user)

if os.geteuid() != 0:
    print(_("Please run this command as root:\n"))
    print("\tsudo howdy " + " ".join(sys.argv[1:]))
    sys.exit(1)

# Beyond this point the user can't change anymore, if we still have root as user we need to abort
if args.user == "root":
    print(
        _(
            "Can't run howdy commands as root, please run this command with the --user flag"
        )
    )
    sys.exit(1)

native_commands = {
    "add": "howdy-add",
    "test": "howdy-test",
}

if args.command in native_commands:
    libdirs = ["/usr/lib/howdy", "/usr/lib64/howdy", "/usr/local/lib/howdy"]
    binary_name = native_commands[args.command]
    binary_path = next(
        (
            os.path.join(libdir, binary_name)
            for libdir in libdirs
            if os.path.isfile(os.path.join(libdir, binary_name))
            and os.access(os.path.join(libdir, binary_name), os.X_OK)
        ),
        "",
    )
    if binary_path == "":
        print(_("Missing native command binary: ") + binary_name)
        sys.exit(1)

    native_args = [binary_path, args.user]
    native_args.extend(args.arguments)
    if args.plain:
        native_args.append("--plain")
    if args.y:
        native_args.append("-y")

    os.execv(binary_path, native_args)

if args.command == "add":
    import cli.add
elif args.command == "clear":
    import cli.clear
elif args.command == "config":
    import cli.config
elif args.command == "disable":
    import cli.disable
elif args.command == "download-models":
    import cli.download_models
elif args.command == "list":
    import cli.list
elif args.command == "remove":
    import cli.remove
elif args.command == "set":
    import cli.set
elif args.command == "snapshot":
    import cli.snap
elif args.command == "test":
    import cli.test
else:
    print("Howdy-Next 1.0.0")
