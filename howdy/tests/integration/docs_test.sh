#!/bin/sh

set -eu

build_dir=$1
source_dir=$2
man1=$3
man8=$4
config_dir=$5
config_path=$6
models_dir=$7
user_models_dir=$8
auth_helper_path=$9

reference_dir="$build_dir/generated/docs"

require_nonempty_file() {
	path=$1
	if [ ! -s "$path" ]; then
		printf '%s\n' "Missing or empty generated documentation: $path" >&2
		exit 1
	fi
}

require_contains() {
	path=$1
	text=$2
	if ! grep -F -- "$text" "$path" >/dev/null; then
		printf '%s\n' "Generated documentation lacks expected text '$text': $path" >&2
		exit 1
	fi
}

require_exactly_once() {
	path=$1
	text=$2
	count=$(grep -F -- "$text" "$path" | wc -l)
	if [ "$count" -ne 1 ]; then
		printf '%s\n' "Expected exactly one '$text' in $path (found $count)" >&2
		exit 1
	fi
}

require_roff_path() {
	path=$1
	manual=$2
	escaped_path=$(printf '%s\n' "$path" | sed 's/-/\\-/g')
	require_contains "$manual" "$escaped_path"
}

require_nonempty_file "$man1"
require_nonempty_file "$man8"
for fragment in howdy-commands.roff howdy-options.roff pam-workarounds.roff; do
	require_nonempty_file "$reference_dir/$fragment"
done

man1_date=$(sed -n '1s/^\.TH HOWDY 1 "\([^"]*\)".*/\1/p' "$man1")
man8_date=$(sed -n '1s/^\.TH PAM_HOWDY 8 "\([^"]*\)".*/\1/p' "$man8")
if ! printf '%s\n' "$man1_date" | grep -Eq '^[0-9]{4}-[0-9]{2}-[0-9]{2}$' ||
	[ "$man1_date" != "$man8_date" ]; then
	printf '%s\n' "Man pages do not share an ISO UTC date: '$man1_date' '$man8_date'" >&2
	exit 1
fi
if grep -E '[0-9]{4}-[0-9]{2}-[0-9]{2}' \
	"$source_dir/howdy/howdy.1.in" "$source_dir/pam/pam_howdy.8.in" >/dev/null; then
	printf '%s\n' 'Man-page templates contain a hardcoded calendar date' >&2
	exit 1
fi

for command in add clear config disable download-models list remove set snapshot test version; do
	escaped_command=$command
	case "$command" in
		download-models) escaped_command='download\-models' ;;
	esac
	label="\\&\\fB${escaped_command}\\fR"
	require_exactly_once "$reference_dir/howdy-commands.roff" "$label"
	require_exactly_once "$man1" "$label"
done

for option in '\-U, \-\-user USER' '\-\-plain' '\-y' '\-h, \-\-help'; do
	label="\\&\\fB${option}\\fR"
	require_exactly_once "$reference_dir/howdy-options.roff" "$label"
	require_exactly_once "$man1" "$label"
done

for value in input native native-input; do
	escaped_value=$value
	case "$value" in
		native-input) escaped_value='native\-input' ;;
	esac
	label="\\&\\fBworkaround=${escaped_value}\\fR"
	require_exactly_once "$reference_dir/pam-workarounds.roff" "$label"
	require_exactly_once "$man8" "$label"
done
require_contains "$man8" 'Workaround is off when workaround= is omitted.'
if grep -F 'workaround=off' "$reference_dir/pam-workarounds.roff" >/dev/null; then
	printf '%s\n' 'Invalid workaround=off token was generated' >&2
	exit 1
fi

require_roff_path "$config_path" "$man1"
require_roff_path "$models_dir/" "$man1"
require_roff_path "$user_models_dir/" "$man1"
require_roff_path "$config_dir" "$man8"
require_roff_path "$config_path" "$man8"
require_roff_path "$user_models_dir/" "$man8"
require_roff_path "$auth_helper_path" "$man8"

if [ -e "$source_dir/howdy/howdy.1" ] || [ -e "$source_dir/pam/pam_howdy.8" ]; then
	printf '%s\n' 'Checked-in generated man page remains in source tree' >&2
	exit 1
fi
if grep -E '^\|[[:space:]]*Command[[:space:]]*\|' "$source_dir/README.md" >/dev/null; then
	printf '%s\n' 'README still contains exhaustive command table' >&2
	exit 1
fi
require_contains "$source_dir/README.md" 'See `howdy(1)` for the complete command and option reference.'
require_contains "$source_dir/README.md" 'See `pam_howdy(8)` for PAM configuration and workaround modes.'
if grep -E 'workaround=(off|input|native|native-input)' "$source_dir/pam/README.md" >/dev/null; then
	printf '%s\n' 'pam/README.md still contains workaround reference values' >&2
	exit 1
fi
require_contains "$source_dir/pam/README.md" 'pam_howdy(8)'

if [ -f "$build_dir/install_manifest.txt" ]; then
	for private_file in howdy_docs_generator howdy-commands.roff howdy-options.roff pam-workarounds.roff; do
		if grep -F -- "$private_file" "$build_dir/install_manifest.txt" >/dev/null; then
			printf '%s\n' "Private documentation artifact appears in install manifest: $private_file" >&2
			exit 1
		fi
	done
fi

if command -v mandoc >/dev/null 2>&1; then
	mandoc -T utf8 "$man1" >/dev/null
	mandoc -T utf8 "$man8" >/dev/null
elif command -v groff >/dev/null 2>&1; then
	groff -man -Tutf8 "$man1" >/dev/null
	groff -man -Tutf8 "$man8" >/dev/null
else
	printf '%s\n' 'Man syntax check skipped: no mandoc or groff available'
fi
