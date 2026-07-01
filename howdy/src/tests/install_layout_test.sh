#!/bin/sh

set -eu

installed=$(meson introspect "$1" --installed)
prefix=${2%/}

resolve_dir() {
	case "$2" in
	/*) printf '%s\n' "$2" ;;
	*) printf '%s/%s\n' "$1" "$2" ;;
	esac
}

bindir=$(resolve_dir "$prefix" "$3")
libdir=$(resolve_dir "$prefix" "$4")

howdy_path="$bindir/howdy"
compare_path="$libdir/howdy/howdy-compare"
auth_helper_path="$libdir/howdy/howdy-auth-helper"
command_dir="$libdir/howdy"

require_path() {
	if ! printf '%s\n' "$installed" | grep -Fq '"'"$1"'"'; then
		echo "Missing installed executable: $1" >&2
		exit 1
	fi
}

require_path "$howdy_path"
require_path "$compare_path"
require_path "$auth_helper_path"

for command in add clear config disable download-models list remove set snapshot test; do
	if printf '%s\n' "$installed" | grep -Fq '"'"$command_dir/howdy-$command"'"'; then
		echo "Obsolete standalone CLI command remains installed: howdy-$command" >&2
		exit 1
	fi
done
