#!/bin/sh

set -eu

build_dir=$1
configured_prefix=$2
bindir=$3
libexecdir=$4
datadir=$5
localedir=$6
mandir=$7
helper_dir=$8
pam_dir=$9
config_dir=${10}
config_path=${11}
models_dir=${12}
user_models_dir=${13}

strip_trailing_slashes() {
	path=$1
	while [ "$path" != "/" ] && [ "${path%/}" != "$path" ]; do
		path=${path%/}
	done
	printf '%s\n' "$path"
}

resolve_dir() {
	base=$1
	dir=$2
	case "$dir" in
		/*) strip_trailing_slashes "$dir" ;;
		*)
			base=$(strip_trailing_slashes "$base")
			if [ "$base" = "/" ]; then
				printf '/%s\n' "${dir%/}"
			else
				printf '%s/%s\n' "$base" "${dir%/}"
			fi
			;;
	esac
}

normalize_mode() {
	mode=$1
	while [ "$mode" != "0" ] && [ "${mode#0}" != "$mode" ]; do
		mode=${mode#0}
	done
	printf '%s\n' "$mode"
}

require_file() {
	path=$1
	if [ ! -f "$path" ]; then
		echo "Missing installed regular file: $path" >&2
		exit 1
	fi
}

require_directory() {
	path=$1
	if [ ! -d "$path" ]; then
		echo "Missing installed directory: $path" >&2
		exit 1
	fi
}

require_mode() {
	path=$1
	expected_mode=$2
	actual_mode=$(normalize_mode "$(stat -c '%a' "$path")")
	if [ "$actual_mode" != "$expected_mode" ]; then
		echo "Installed path has wrong mode: $path (expected ${expected_mode}, got ${actual_mode})" >&2
		exit 1
	fi
}

require_executable() {
	path=$1
	if [ ! -x "$path" ]; then
		echo "Installed executable is not executable: $path" >&2
		exit 1
	fi
}

count_exact_paths() {
	root=$1
	type=$2
	target=$3
	count=0
	while IFS= read -r found; do
		if [ "$found" = "$target" ]; then
			count=$((count + 1))
		fi
	done <<EOF
$(find "$root" -type "$type" -print)
EOF
	printf '%s\n' "$count"
}

require_exact_path_once() {
	root=$1
	type=$2
	target=$3
	count=$(count_exact_paths "$root" "$type" "$target")
	if [ "$count" -ne 1 ]; then
		echo "Expected exactly one installed path at $target (found ${count})" >&2
		exit 1
	fi
}

clean_stage() {
	rm -rf "$1"
}

install_same_prefix() {
	same_stage=$1
	same_prefix=$2
	clean_stage "$same_stage"
	DESTDIR="$same_stage" cmake --install "$build_dir" --prefix "$same_prefix" >/dev/null
	verify_layout "$same_stage"
}

test_literal_prefix() {
	case_dir=$1
	literal_prefix=$2
	mkdir -p "$case_dir"
	cp "$build_dir/cmake_install_prefix_guard.cmake" "$case_dir/"
	printf '%s' "$literal_prefix" >"$case_dir/cmake_install_prefix.txt"
	cmake -D "CMAKE_INSTALL_PREFIX=$literal_prefix" \
		-P "$case_dir/cmake_install_prefix_guard.cmake" >/dev/null
}

verify_obsolete_commands_absent() {
	stage=$1
	bindir_path=$2
	helper_dir_path=$3
	for command in add clear config disable download-models list remove set snapshot test; do
		for obsolete_path in \
			"${stage}${bindir_path}/howdy-${command}" \
			"${stage}${helper_dir_path}/howdy-${command}"; do
			if [ -e "$obsolete_path" ]; then
				echo "Obsolete standalone CLI command remains installed: $obsolete_path" >&2
				exit 1
			fi
		done
	done
}

verify_layout() {
	stage=$1
	bindir_path=$(resolve_dir "$configured_prefix" "$bindir")
	datadir_path=$(resolve_dir "$configured_prefix" "$datadir")
	localedir_path=$(resolve_dir "$configured_prefix" "$localedir")
	mandir_path=$(resolve_dir "$configured_prefix" "$mandir")
	libexecdir_path=$(resolve_dir "$configured_prefix" "$libexecdir")
	helper_dir_path=$(resolve_dir "$configured_prefix" "$helper_dir")
	if [ "$helper_dir_path" != "${libexecdir_path}/howdy" ]; then
		echo "Computed helper directory differs from libexecdir/howdy: $helper_dir_path" >&2
		exit 1
	fi
	pam_dir_path=$(resolve_dir "$configured_prefix" "$pam_dir")
	config_dir_path="${stage}$(resolve_dir "$configured_prefix" "$config_dir")"
	config_path_path=$(resolve_dir "$configured_prefix" "$config_path")
	models_dir_path=$(resolve_dir "$configured_prefix" "$models_dir")
	user_models_dir_path=$(resolve_dir "$configured_prefix" "$user_models_dir")

	howdy_path="${stage}${bindir_path}/howdy"
	compare_path="${stage}${helper_dir_path}/howdy-compare"
	auth_helper_path="${stage}${helper_dir_path}/howdy-auth-helper"
	pam_path="${stage}${pam_dir_path}/pam_howdy.so"
	config_file_path="${stage}${config_path_path}"
	models_path="${stage}${models_dir_path}"
	user_models_path="${stage}${user_models_dir_path}"
	completion_path="${stage}${datadir_path}/bash-completion/completions/howdy"
	translation_path="${stage}${localedir_path}/th/LC_MESSAGES/howdy.mo"
	man1_path="${stage}${mandir_path}/man1/howdy.1"
	man8_path="${stage}${mandir_path}/man8/pam_howdy.8"

	for file_path in \
		"$howdy_path" \
		"$compare_path" \
		"$auth_helper_path" \
		"$pam_path" \
		"$config_file_path" \
		"$completion_path" \
		"$translation_path" \
		"$man1_path" \
		"$man8_path"; do
		require_file "$file_path"
		require_exact_path_once "$stage" f "$file_path"
	done
	if [ "$(find "$stage" -type f -name howdy_docs_generator -print | wc -l)" -ne 0 ]; then
		echo "Documentation generator was installed" >&2
		exit 1
	fi
	require_executable "$howdy_path"
	require_executable "$compare_path"
	require_executable "$auth_helper_path"

	for directory_path in "$models_path" "$user_models_path"; do
		require_directory "$directory_path"
		require_exact_path_once "$stage" d "$directory_path"
	done

	require_directory "$config_dir_path"
	require_exact_path_once "$stage" d "$config_dir_path"
	require_mode "$auth_helper_path" 4755
	require_mode "$config_file_path" 640
	require_mode "$config_dir_path" 750
	require_mode "$user_models_path" 750
	verify_obsolete_commands_absent "$stage" "$bindir_path" "$helper_dir_path"
}

configured_prefix=$(strip_trailing_slashes "$configured_prefix")
if [ "$(resolve_dir / bin)" != "/bin" ] || [ "$(resolve_dir / /etc)" != "/etc" ]; then
	echo "Root install prefix produced invalid destination path" >&2
	exit 1
fi
stage="$build_dir/install layout root-ทดสอบ"
same_prefix_stage="$build_dir/install same prefix root-ทดสอบ"
override_stage="$build_dir/install override root-ทดสอบ"
override_log="$build_dir/install-prefix-override.log"
direct_pam_stage="$build_dir/direct pam install root-ทดสอบ"
direct_howdy_stage="$build_dir/direct howdy install root-ทดสอบ"
direct_pam_po_stage="$build_dir/direct pam-po install root-ทดสอบ"
direct_pam_log="$build_dir/direct-pam-install-prefix.log"
direct_howdy_log="$build_dir/direct-howdy-install-prefix.log"
direct_pam_po_log="$build_dir/direct-pam-po-install-prefix.log"
literal_prefix_root="$build_dir/install literal prefix cases"

cleanup() {
	rm -rf "$stage" "$same_prefix_stage" "$override_stage" "$override_log" \
		"${override_log}.out" "$direct_pam_stage" "$direct_howdy_stage" \
		"$direct_pam_po_stage" "$direct_pam_log" \
		"${direct_pam_log}.out" "$direct_howdy_log" "${direct_howdy_log}.out" \
		"$direct_pam_po_log" "${direct_pam_po_log}.out" "$literal_prefix_root"
}
trap cleanup 0 1 2 3 15

test_literal_prefix "$literal_prefix_root/dollar" '/opt/howdy/$literal'
test_literal_prefix "$literal_prefix_root/braced-dollar" '/opt/howdy/${UNDEFINED}'
test_literal_prefix "$literal_prefix_root/quote" '/opt/howdy/"quoted"'

clean_stage "$stage"
DESTDIR="$stage" cmake --install "$build_dir" >/dev/null
verify_layout "$stage"

config_file_path="$(resolve_dir "$configured_prefix" "$config_path")"
installed_config_path="${stage}${config_file_path}"
config_marker='install-layout-config-preservation-marker'
printf '%s\n' "$config_marker" >"$installed_config_path"

DESTDIR="$stage" cmake --install "$build_dir" >/dev/null
verify_layout "$stage"
if [ "$(cat "$installed_config_path")" != "$config_marker" ]; then
	echo "Second staged install overwrote existing config.ini: $installed_config_path" >&2
	exit 1
fi

doubled_stage="${stage}${stage}"
doubled_config_path="${stage}${stage}${config_file_path}"
doubled_user_models_path="${stage}${stage}$(resolve_dir "$configured_prefix" "$user_models_dir")"
if [ -e "$doubled_stage" ]; then
	echo "Nested duplicate staging root exists: $doubled_stage" >&2
	exit 1
fi
if [ -e "$doubled_config_path" ]; then
	echo "Config installed under doubled DESTDIR prefix: $doubled_config_path" >&2
	exit 1
fi
if [ -e "$doubled_user_models_path" ]; then
	echo "User models installed under doubled DESTDIR prefix: $doubled_user_models_path" >&2
	exit 1
fi
if [ "$(count_exact_paths "$stage" f "$installed_config_path")" -ne 1 ]; then
	echo "Expected exactly one installed config.ini under staged tree" >&2
	exit 1
fi
config_count=0
while IFS= read -r found_config; do
	config_count=$((config_count + 1))
done <<EOF
$(find "$stage" -type f -name config.ini -print)
EOF
if [ "$config_count" -ne 1 ]; then
	echo "Expected exactly one config.ini in staged tree (found ${config_count})" >&2
	exit 1
fi
require_exact_path_once "$stage" d "${stage}$(resolve_dir "$configured_prefix" "$user_models_dir")"

if [ "$configured_prefix" = "/" ]; then
	install_same_prefix "$same_prefix_stage" "/."
else
	install_same_prefix "$same_prefix_stage" "${configured_prefix}/"
	install_same_prefix "$same_prefix_stage" "${configured_prefix}//"
	install_same_prefix "$same_prefix_stage" "${configured_prefix}/."
	install_same_prefix "$same_prefix_stage" "//${configured_prefix#/}"
fi

clean_stage "$override_stage"
different_prefix="$build_dir/install-prefix-override-target"
while [ "$(strip_trailing_slashes "$different_prefix")" = "$configured_prefix" ]; do
	different_prefix="${different_prefix}-different"
done

reject_direct_prefix() {
	install_dir=$1
	direct_stage=$2
	direct_log=$3
	clean_stage "$direct_stage"
	if DESTDIR="$direct_stage" cmake --install "$install_dir" --prefix "$different_prefix" \
		>"${direct_log}.out" 2>"$direct_log"; then
		echo "Direct install unexpectedly accepted prefix override: $install_dir" >&2
		exit 1
	fi
	if ! grep -F 'Reconfigure with -DCMAKE_INSTALL_PREFIX=<path>' "$direct_log" >/dev/null \
		|| ! grep -F 'cmake --install --prefix' "$direct_log" >/dev/null \
		|| ! grep -F 'runtime paths are generated at configure time' "$direct_log" >/dev/null; then
		echo "Direct install omitted unsupported-prefix diagnostic: $install_dir" >&2
		cat "$direct_log" >&2
		exit 1
	fi
	if [ -e "$direct_stage" ]; then
		echo "Direct install created artifacts before failing: $direct_stage" >&2
		find "$direct_stage" -print >&2
		exit 1
	fi
}

reject_direct_prefix "$build_dir/pam" "$direct_pam_stage" "$direct_pam_log"
reject_direct_prefix "$build_dir/howdy" "$direct_howdy_stage" "$direct_howdy_log"
reject_direct_prefix "$build_dir/pam/po" "$direct_pam_po_stage" "$direct_pam_po_log"

if DESTDIR="$override_stage" cmake --install "$build_dir" --prefix "$different_prefix" \
	>"${override_log}.out" 2>"$override_log"; then
	echo "Install-time prefix override unexpectedly succeeded: $different_prefix" >&2
	exit 1
fi
if ! grep -F 'Reconfigure with -DCMAKE_INSTALL_PREFIX=<path>' "$override_log" >/dev/null \
	|| ! grep -F 'cmake --install --prefix' "$override_log" >/dev/null \
	|| ! grep -F 'runtime paths are generated at configure time' "$override_log" >/dev/null; then
	echo "Prefix override failure omitted unsupported-prefix diagnostic" >&2
	cat "$override_log" >&2
	exit 1
fi
if [ -e "$override_stage" ]; then
	echo "Prefix override created files before failing: $override_stage" >&2
	find "$override_stage" -print >&2
	exit 1
fi
