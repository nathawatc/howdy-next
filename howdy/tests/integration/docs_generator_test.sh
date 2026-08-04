#!/bin/sh

set -eu

generator=$1
root=$2
rm -rf "$root"
mkdir -p "$root"
trap 'rm -rf "$root"' EXIT

if "$generator" >/dev/null 2>&1; then
	printf '%s\n' 'Invalid generator invocation unexpectedly succeeded' >&2
	exit 1
else
	status=$?
fi
if [ "$status" -ne 2 ]; then
	printf '%s\n' "Invalid generator invocation returned $status, expected 2" >&2
	exit 1
fi

output_dir="$root/output"
mkdir -p "$output_dir"
"$generator" --output-dir "$output_dir"
for fragment in howdy-commands.roff howdy-options.roff pam-workarounds.roff; do
	if [ ! -s "$output_dir/$fragment" ]; then
		printf '%s\n' "Missing or empty generated fragment: $fragment" >&2
		exit 1
	fi
done

blocked_parent="$root/blocked"
printf '%s\n' 'not a directory' >"$blocked_parent"
if "$generator" --output-dir "$blocked_parent/output" >/dev/null 2>&1; then
	printf '%s\n' 'Filesystem failure unexpectedly succeeded' >&2
	exit 1
fi
