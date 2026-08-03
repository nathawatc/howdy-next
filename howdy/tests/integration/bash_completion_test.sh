#!/usr/bin/env bash

set -euo pipefail

howdy_binary=$1
completion_file=$2

bash -n "$completion_file"

howdy() {
	"$howdy_binary" "$@"
}

source "$completion_file"

assert_single_completion() {
	local label=$1
	local expected=$2
	shift 2

	COMP_WORDS=("$@")
	COMP_CWORD=$((${#COMP_WORDS[@]} - 1))
	_howdy

	if (( ${#COMPREPLY[@]} != 1 )); then
		printf 'Unexpected completion count for %s\n' "$label" >&2
		printf 'Expected one result, got %d\n' "${#COMPREPLY[@]}" >&2
		exit 1
	fi
	if [[ "${COMPREPLY[0]}" != "$expected" ]]; then
		printf 'Unexpected completion for %s\n' "$label" >&2
		printf 'Expected: %s\nActual: %s\n' "$expected" "${COMPREPLY[0]}" >&2
		exit 1
	fi
}

assert_single_completion 'command' disable howdy dis
assert_single_completion 'short option before command' disable howdy -y dis
assert_single_completion 'plain option before command' disable howdy --plain dis
assert_single_completion 'short user option before command' disable howdy -U alice dis
assert_single_completion 'long user option before command' disable howdy --user alice dis
assert_single_completion 'disable false value' false howdy disable f
assert_single_completion 'disable true value' true howdy disable t
