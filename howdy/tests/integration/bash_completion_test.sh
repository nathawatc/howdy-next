#!/usr/bin/env bash

set -euo pipefail

howdy_binary=$1
completion_file=$2
howdy_command=("${@:3}" "$howdy_binary")

bash -n "$completion_file"

completion_cache_dir=$(mktemp -d)
trap 'rm -rf -- "$completion_cache_dir"' EXIT

howdy() {
	if [[ ${1-} != __complete ]]; then
		"${howdy_command[@]}" "$@"
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
		IFS= read -r status < "$cache_status"
		return "$status"
	fi

	if output=$("${howdy_command[@]}" "$@"); then
		status=0
	else
		status=$?
	fi
	if [[ -n $output ]]; then
		printf '%s\n' "$output" > "$cache_output"
	else
		: > "$cache_output"
	fi
	printf '%s\n' "$status" > "$cache_status"
	cat "$cache_output"
	return "$status"
}

current_user=$(id -un)
if ! type complete > /dev/null 2>&1; then
	complete() { :; }
fi
if ! type compgen > /dev/null 2>&1; then
	compgen() {
		if (($# != 3)) || [[ $1 != -u || $2 != -- ]]; then
			return 2
		fi
		if [[ $current_user == "$3"* ]]; then
			printf '%s\n' "$current_user"
		fi
	}
fi
# shellcheck source=/dev/null
source "$completion_file"

while IFS= read -r -d '' word_count; do
	[[ $word_count =~ ^[0-9]+$ ]] || exit 2
	COMP_WORDS=()
	for ((index = 0; index < word_count; ++index)); do
		IFS= read -r -d '' word || exit 2
		COMP_WORDS+=("$word")
	done
	COMP_CWORD=$((${#COMP_WORDS[@]} - 1))
	_howdy
	printf '%s\0' "${#COMPREPLY[@]}"
	for completion in "${COMPREPLY[@]}"; do
		printf '%s\0' "$completion"
	done
done
