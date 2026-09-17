#!/usr/bin/env bash

set -euo pipefail

howdy_binary=$1
completion_file=$2

bash -n "$completion_file"

completion_cache_dir=$(mktemp -d)
trap 'rm -rf -- "$completion_cache_dir"' EXIT

howdy() {
	if [[ ${1-} != __complete ]]; then
		"$howdy_binary" "$@"
		return
	fi

	local cache_key cache_output cache_status output status
	printf -v cache_key '%q ' "$@"
	cache_key=$(printf '%s' "$cache_key" | sha256sum)
	cache_key=${cache_key%% *}
	cache_output="$completion_cache_dir/$cache_key.output"
	cache_status="$completion_cache_dir/$cache_key.status"
	if [[ -f $cache_status ]]; then
		cat "$cache_output"
		IFS= read -r status <"$cache_status"
		return "$status"
	fi

	if output=$("$howdy_binary" "$@"); then
		status=0
	else
		status=$?
	fi
	if [[ -n $output ]]; then
		printf '%s\n' "$output" >"$cache_output"
	else
		: >"$cache_output"
	fi
	printf '%s\n' "$status" >"$cache_status"
	cat "$cache_output"
	return "$status"
}

current_user=$(id -un)
if ! type complete >/dev/null 2>&1; then
	complete() { :; }
fi
if ! type compgen >/dev/null 2>&1; then
	compgen() {
		if (( $# != 3 )) || [[ $1 != -u || $2 != -- ]]; then
			return 2
		fi
		if [[ "$current_user" == "$3"* ]]; then
			printf '%s\n' "$current_user"
		fi
	}
fi
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
assert_single_completion 'short option before compatible command' add howdy -y a
assert_single_completion 'plain option before compatible command' list howdy --plain l
assert_single_completion 'combined options before compatible command' add howdy --plain -y a
assert_single_completion 'short user option before compatible command' test howdy -U alice t
assert_single_completion 'long user option before compatible command' test howdy --user alice t
assert_completion_list 'short options' $'-U\n--user\n--plain\n-y\n-h\n--help' howdy -
assert_completion_list 'long options' $'--user\n--plain\n--help' howdy --
assert_completion_list 'post-command short options' $'-U\n--user\n--plain\n-y\n-h\n--help' howdy add -
assert_completion_list 'post-command long options' $'--user\n--plain\n--help' howdy add --
assert_single_completion 'test device option' --device howdy test --d
assert_single_completion 'long option' --plain howdy --p
assert_single_completion 'short user value' "$current_user" howdy -U "$current_user"
assert_single_completion 'user value' "$current_user" howdy --user "$current_user"
assert_contains_completion 'empty user value' "$current_user" howdy --user ""
assert_contains_completion 'post-command short user value' "$current_user" howdy add -U ""
assert_contains_completion 'post-command user value' "$current_user" howdy add --user ""

assert_completion_list 'empty disable value' $'false\ntrue' howdy disable ""
assert_single_completion 'disable false value' false howdy disable f
assert_single_completion 'disable true value' true howdy disable t

assert_contains_completion 'set key' detection_notice howdy set ""
assert_completion_list 'set key prefix' $'sface_metric\nsface_threshold' howdy set sfa
assert_completion_list 'set boolean value' $'false\ntrue' howdy set detection_notice ""
assert_completion_list 'set choice values' $'cosine\nl2\nl2norm' howdy set sface_metric ""

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

assert_no_completion 'yes option excludes incompatible command' howdy -y dis
assert_no_completion 'plain option excludes incompatible command' howdy --plain dis
assert_no_completion 'combined options exclude partially compatible command' howdy --plain -y l
assert_no_completion 'short user option excludes incompatible command' howdy -U alice dis
assert_no_completion 'long user option excludes incompatible command' howdy --user alice dis
assert_no_completion 'typed yes option disables incompatible command completion' howdy disable -y ""
assert_no_completion 'typed plain option disables incompatible command completion' howdy disable --plain ""
assert_no_completion 'typed user option disables incompatible command completion' howdy disable -U alice ""
assert_no_completion 'pre-command incompatible option disables value completion' howdy -y disable ""
assert_no_completion 'help terminates command completion' howdy add --help -
assert_no_completion 'unknown command option disables completion' howdy list --bogus -
assert_no_completion 'foreign command option disables completion' howdy list --device /dev/video0 -
assert_no_completion 'unknown option after command option disables completion' howdy test --device /dev/video0 --bogus -
assert_no_completion 'duplicate command option disables completion' howdy test --device /dev/video0 --device /dev/video1 ""
assert_no_completion 'surplus positional disables completion' howdy list bob -
assert_no_completion 'surplus positional after option terminator disables completion' howdy list -- bob -
assert_no_completion 'surplus positional after global option disables completion' howdy list --plain bob -
assert_no_completion 'surplus positional after command option disables completion' howdy test --device /dev/video0 extra -
assert_no_completion 'empty committed device value disables completion' howdy test --device "" -
assert_no_completion 'empty committed user value disables completion' howdy -U "" list -
assert_completion_list 'disable help options' $'-h\n--help' howdy disable -
assert_no_completion 'disable rejects short user value' howdy disable -U ""
assert_no_completion 'disable rejects user value' howdy disable --user ""
assert_no_completion 'end-of-options suppresses user completion' howdy add -- -U ""
assert_no_completion 'used test device option is not repeated' howdy test --device /dev/video0 --d
assert_no_completion 'set numeric value' howdy set timeout ""
assert_no_completion 'set floating-point value' howdy set sface_threshold ""
assert_no_completion 'version has no values' howdy version ""
assert_no_completion 'help disables completion after command' howdy config --help "f"
assert_no_completion 'help suppresses user value' howdy --help -U ""
