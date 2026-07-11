#!/bin/sh

set -eu

build_dir=$1
prefix=${2%/}
stage="$build_dir/install-layout-root"

resolve_dir() {
	case "$2" in
	/*) printf '%s\n' "$2" ;;
	*) printf '%s/%s\n' "$1" "$2" ;;
	esac
}

rm -rf "$stage"
DESTDIR="$stage" cmake --install "$build_dir" >/dev/null

bindir=$(resolve_dir "$prefix" "$3")
libexecdir=$(resolve_dir "$prefix" "$4")

howdy_path="$stage$bindir/howdy"
compare_path="$stage$libexecdir/howdy/howdy-compare"
auth_helper_path="$stage$libexecdir/howdy/howdy-auth-helper"
command_dir="$stage$libexecdir/howdy"

require_executable() {
	if [ ! -x "$1" ]; then
		echo "Missing installed executable: ${1#"$stage"}" >&2
		exit 1
	fi
}

require_executable "$howdy_path"
require_executable "$compare_path"
require_executable "$auth_helper_path"

for command in add clear config disable download-models list remove set snapshot test; do
	if [ -e "$command_dir/howdy-$command" ]; then
		echo "Obsolete standalone CLI command remains installed: howdy-$command" >&2
		exit 1
	fi
done
