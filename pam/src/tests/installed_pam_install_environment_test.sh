#!/bin/sh

set -eu

harness=$1
shift
install_prefix=$2
fixture=$(mktemp -d /tmp/howdy-install-environment.XXXXXX)
hostile_destdir=$fixture/destdir
redirect_target=$fixture/redirect-target

cleanup() {
	rm -rf -- "$fixture"
}
trap cleanup 0 1 2 3 15

chmod 0777 "$fixture"
mkdir -m 0777 -- "$hostile_destdir" "$redirect_target"
ln -s -- "$redirect_target" "$hostile_destdir/opt"

DESTDIR=$hostile_destdir CMAKE_INSTALL_MODE=REL_SYMLINK bash "$harness" "$@"

[ ! -e "$install_prefix" ] || {
	echo "Isolated installation remains after environment regression" >&2
	exit 1
}
[ -L "$hostile_destdir/opt" ] || {
	echo "Hostile DESTDIR fixture symlink changed" >&2
	exit 1
}
[ -z "$(find "$hostile_destdir" -mindepth 1 ! -path "$hostile_destdir/opt" -print -quit)" ] || {
	echo "Installation wrote below hostile DESTDIR" >&2
	exit 1
}
[ -z "$(find "$redirect_target" -mindepth 1 -print -quit)" ] || {
	echo "Installation followed hostile DESTDIR symlink" >&2
	exit 1
}

printf 'Install environment isolation: passed\n'
