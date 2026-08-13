#!/usr/bin/env bash

set -euo pipefail

howdy_binary=$1
completion_file=$2

bash -n "$completion_file"

howdy() {
	"$howdy_binary" "$@"
}

source "$completion_file"

current_user=$(id -un)

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

assert_completion_list() {
	local label=$1
	local expected=$2
	shift 2

	COMP_WORDS=("$@")
	COMP_CWORD=$((${#COMP_WORDS[@]} - 1))
	_howdy

	local actual=""
	if (( ${#COMPREPLY[@]} > 0 )); then
		actual=$(printf '%s\n' "${COMPREPLY[@]}")
	fi
	if [[ "$actual" != "$expected" ]]; then
		printf 'Unexpected completion for %s\n' "$label" >&2
		printf 'Expected:\n%s\nActual:\n%s\n' "$expected" "$actual" >&2
		exit 1
	fi
}

assert_contains_completion() {
	local label=$1
	local expected=$2
	shift 2

	COMP_WORDS=("$@")
	COMP_CWORD=$((${#COMP_WORDS[@]} - 1))
	_howdy

	for completion in "${COMPREPLY[@]}"; do
		if [[ "$completion" == "$expected" ]]; then
			return 0
		fi
	done
	printf 'Missing completion for %s: %s\n' "$label" "$expected" >&2
	exit 1
}

assert_completion_list 'commands' $'add\nclear\nconfig\ndisable\ndownload-models\nlist\nremove\nset\nsnapshot\ntest\nversion' howdy ""
assert_single_completion 'command' disable howdy dis
assert_single_completion 'short option before command' disable howdy -y dis
assert_single_completion 'plain option before command' disable howdy --plain dis
assert_single_completion 'short user option before command' disable howdy -U alice dis
assert_single_completion 'long user option before command' disable howdy --user alice dis
assert_completion_list 'short options' $'-U\n--user\n--plain\n-y\n-h\n--help' howdy -
assert_completion_list 'long options' $'--user\n--plain\n--help' howdy --
assert_completion_list 'post-command short options' $'-U\n--user\n--plain\n-y' howdy disable -
assert_completion_list 'post-command long options' $'--user\n--plain' howdy disable --
assert_single_completion 'long option' --plain howdy --p
assert_single_completion 'short user value' "$current_user" howdy -U "$current_user"
assert_single_completion 'user value' "$current_user" howdy --user "$current_user"
assert_contains_completion 'empty user value' "$current_user" howdy --user ""
assert_contains_completion 'post-command short user value' "$current_user" howdy disable -U ""
assert_contains_completion 'post-command user value' "$current_user" howdy disable --user ""

assert_completion_list 'empty disable value' $'false\ntrue' howdy disable ""
assert_single_completion 'disable false value' false howdy disable f
assert_single_completion 'disable true value' true howdy disable t

assert_no_completion() {
	local label=$1
	shift

	COMP_WORDS=("$@")
	COMP_CWORD=$((${#COMP_WORDS[@]} - 1))
	_howdy

	if (( ${#COMPREPLY[@]} != 0 )); then
		printf 'Unexpected completion for %s\n' "$label" >&2
		exit 1
	fi
}

assert_no_completion 'version has no values' howdy version ""
assert_no_completion 'help is not an option after command' howdy config --help ""
assert_no_completion 'help disables completion after command' howdy config --help "f"
assert_no_completion 'help suppresses user value' howdy --help -U ""
