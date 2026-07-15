#!/usr/bin/env bash
set -euo pipefail

skip() { printf 'SKIP: %s\n' "$1"; exit 77; }
fail() { printf 'FAIL: %s\n' "$1" >&2; exit 1; }
as_user() { setpriv --reuid "$selected_uid" --regid "$selected_gid" --init-groups "$@"; }
validate_install_prefix() {
	local input=$1 normalized parent basename canonical_parent
	validated_install_prefix=
	[[ $input == /* ]] || return 1
	normalized=$(realpath -m -- "$input") || return 1
	[[ $input == "$normalized" ]] || return 1
	parent=${normalized%/*}
	basename=${normalized##*/}
	[[ $basename != */* && $basename != . && $basename != .. ]] || return 1
	[[ $basename =~ ^howdy-installed-pam-e2e-[A-Za-z0-9._-]+$ ]] || return 1
	canonical_parent=$(realpath -e -- "$parent") || return 1
	[[ $canonical_parent == /opt ]] || return 1
	[[ ! -e $normalized && ! -L $normalized ]] || return 1
	validated_install_prefix=$normalized
}
validate_install_subdir() {
	local input=$1 normalized
	validated_install_subdir=
	[[ $input == /* ]] || return 1
	normalized=$(realpath -m -- "$input") || return 1
	[[ $input == "$normalized" ]] || return 1
	[[ $normalized == "$validated_install_prefix/"* ]] || return 1
	validated_install_subdir=$normalized
}
validate_opt_directory() {
	local canonical mode
	[[ -d /opt && ! -L /opt ]] || return 1
	canonical=$(realpath -e -- /opt) || return 1
	[[ $canonical == /opt ]] || return 1
	[[ $(stat -c '%u' /opt) == 0 ]] || return 1
	mode=$(stat -c '%a' /opt) || return 1
	(( (8#$mode & 0022) == 0 )) || return 1
	as_user test ! -w /opt || return 1
}
validate_installed_path() {
	local input=$1 expected_type=$2 canonical
	validated_installed_path=
	[[ $input == "$validated_install_prefix/"* && ! -L $input ]] || return 1
	if [[ $expected_type == file ]]; then
		[[ -f $input ]] || return 1
	else
		[[ -d $input ]] || return 1
	fi
	canonical=$(realpath -e -- "$input") || return 1
	[[ $canonical == "$validated_install_prefix/"* ]] || return 1
	validated_installed_path=$canonical
}
run_path_validation_tests() {
	local marker=/tmp/howdy-e2e-path-validation-$$
	local valid_prefix=/opt/howdy-installed-pam-e2e-validation-$$
	[[ ! -e $marker ]] || fail "path-validation marker already exists"
	validate_install_prefix "$valid_prefix" || fail "valid isolated prefix rejected"
	for rejected in \
		'/opt/howdy-installed-pam-e2e-x/../../etc' \
		'/opt/howdy-installed-pam-e2e-x/subdir' \
		'/tmp/howdy-installed-pam-e2e-x' \
		'/opt/howdy-installed-pam-e2e-' \
		'/opt//howdy-installed-pam-e2e-x' \
		'/opt/howdy-installed-pam-e2e-x/.'; do
		if validate_install_prefix "$rejected"; then
			fail "unsafe prefix accepted: $rejected"
		fi
	done
	validate_install_prefix "$valid_prefix" || fail "valid isolated prefix rejected after cases"
	for rejected in /etc/howdy /usr/libexec/howdy "$valid_prefix/../etc" "$valid_prefix"; do
		if validate_install_subdir "$rejected"; then
			fail "unsafe install path accepted: $rejected"
		fi
	done
	for accepted in \
		"$valid_prefix/lib/security" \
		"$valid_prefix/libexec/howdy" \
		"$valid_prefix/etc/howdy" \
		"$valid_prefix/etc/howdy/models"; do
		validate_install_subdir "$accepted" || fail "valid install path rejected: $accepted"
	done
	[[ ! -e $marker && ! -e $valid_prefix ]] || fail "path validation mutated filesystem"
	printf 'Path validation: passed without filesystem mutation\n'
}

if [[ ${1:-} == --validate-paths ]]; then
	run_path_validation_tests
	exit 0
fi

[[ $# -eq 7 ]] || fail "expected seven path arguments"
build_dir=$1 raw_install_prefix=$2 raw_pam_dir=$3 raw_helper_dir=$4 raw_config_dir=$5
raw_user_models_dir=$6 driver=$7
validate_install_prefix "$raw_install_prefix" || fail "unsafe isolated install prefix"
install_prefix=$validated_install_prefix
validate_install_subdir "$raw_pam_dir" || fail "unsafe PAM directory"
pam_dir=$validated_install_subdir
validate_install_subdir "$raw_helper_dir" || fail "unsafe helper directory"
helper_dir=$validated_install_subdir
validate_install_subdir "$raw_config_dir" || fail "unsafe config directory"
config_dir=$validated_install_subdir
validate_install_subdir "$raw_user_models_dir" || fail "unsafe user-model directory"
user_models_dir=$validated_install_subdir
unset DESTDIR
unset CMAKE_INSTALL_MODE
snapshot_runtime_dirs() {
	local destination=$1
	: >"$destination"
	[[ -d /run/howdy ]] || return 0
	find /run/howdy -mindepth 1 -maxdepth 1 -type d -name "pam-$selected_uid-*" -printf '%f\n' |
		sort >"$destination"
}
validate_new_runtime_dir() {
	local basename=$1 snapshot=$2 candidate canonical parent mode
	validated_runtime_dir=
	[[ $basename != */* && $basename != . && $basename != .. ]] || return 1
	[[ $basename == pam-"$selected_uid"-* && $basename != pam-"$selected_uid"- ]] || return 1
	grep -Fx "$basename" "$snapshot" >/dev/null && return 1
	candidate=/run/howdy/$basename
	[[ -d $candidate && ! -L $candidate ]] || return 1
	canonical=$(realpath -e -- "$candidate") || return 1
	[[ $canonical == "$candidate" ]] || return 1
	parent=$(realpath -e -- "$canonical/..") || return 1
	[[ $parent == /run/howdy ]] || return 1
	[[ $(stat -c '%u:%g' "$canonical") == 0:0 ]] || return 1
	mode=$(stat -c '%a' "$canonical") || return 1
	(( (8#$mode & 0022) == 0 )) || return 1
	validated_runtime_dir=$canonical
}
cleanup_new_runtime_dirs() {
	local snapshot=$1 basename
	snapshot_runtime_dirs "$runtime_after"
	while IFS= read -r basename; do
		validate_new_runtime_dir "$basename" "$snapshot" || continue
		if [[ -x ${helper:-} ]]; then
			as_user "$helper" cleanup "$validated_runtime_dir" >/dev/null 2>&1 || true
		fi
		[[ ! -e $validated_runtime_dir ]] || rm -rf -- "$validated_runtime_dir"
	done <"$runtime_after"
}

[[ $EUID -eq 0 ]] || skip "root privileges required"
if [[ ${HOWDY_E2E_MOUNT_NAMESPACE:-0} != 1 ]]; then
	command -v unshare >/dev/null || skip "unshare unavailable"
	unshare --mount --propagation private true 2>/dev/null || skip "private mount namespace unavailable"
	exec unshare --mount --propagation private env HOWDY_E2E_MOUNT_NAMESPACE=1 bash "$0" "$@"
fi
selected_user=${HOWDY_E2E_USER:-${SUDO_USER:-}}
[[ -n $selected_user ]] || skip "set HOWDY_E2E_USER or invoke through sudo with valid SUDO_USER"
passwd_record=$(getent passwd "$selected_user") || skip "selected account does not exist"
IFS=: read -r account_name _ selected_uid selected_gid _ _ _ <<<"$passwd_record"
[[ $account_name == "$selected_user" ]] || fail "selected account name mismatch"
[[ $selected_uid =~ ^[0-9]+$ && $selected_uid -gt 0 ]] || skip "selected account must be non-root"
[[ $selected_gid =~ ^[0-9]+$ ]] || skip "selected account has invalid primary GID"
getent group "$selected_gid" >/dev/null || skip "selected account primary group does not exist"
for command in setpriv findmnt getfacl mount umount realpath cmake python3; do
	command -v "$command" >/dev/null || skip "$command unavailable"
done
validate_opt_directory || fail "unsafe /opt directory"

runtime_before=$(mktemp)
runtime_after=$(mktemp)
source_snapshot=$(mktemp)
helper_output_file=$(mktemp)
service_dir= prepared_root= masked_config_dir= cleanup_install_prefix= config_masked=false
runtime_isolated=false helper=
cleanup() {
	status=$?
	trap - EXIT HUP INT TERM
	if $runtime_isolated; then
		cleanup_new_runtime_dirs "$runtime_before"
		snapshot_runtime_dirs "$runtime_after"
		if ! cmp -s "$runtime_before" "$runtime_after"; then
			printf 'FAIL: runtime cleanup left test-created directories\n' >&2
			status=1
		fi
	fi
	if $config_masked; then umount /etc/howdy >/dev/null 2>&1 || true; fi
	if $runtime_isolated; then umount /run >/dev/null 2>&1 || true; fi
	[[ -z ${service_dir:-} ]] || rm -rf -- "$service_dir"
	if [[ -n $cleanup_install_prefix && -d $cleanup_install_prefix &&
		! -L $cleanup_install_prefix &&
		$(realpath -e -- "$cleanup_install_prefix") == "$cleanup_install_prefix" ]]; then
		rm -rf -- "$cleanup_install_prefix"
	fi
	rm -f -- "$runtime_before" "$runtime_after" "$source_snapshot" "$helper_output_file"
	exit "$status"
}
trap cleanup EXIT
trap 'exit 128' HUP INT TERM

mount -t tmpfs -o mode=0755,nodev,nosuid tmpfs /run || skip "cannot isolate runtime root"
runtime_isolated=true
[[ ! -e /run/howdy ]] || fail "isolated runtime root is not initially absent"

[[ ! -e $install_prefix ]] || fail "isolated install prefix already exists"
mkdir -m 0755 -- "$install_prefix"
chown 0:0 "$install_prefix"
chmod 0755 "$install_prefix"
created_install_prefix=$(realpath -e -- "$install_prefix") || fail "cannot canonicalize install prefix"
[[ $created_install_prefix == "$install_prefix" && -d $install_prefix && ! -L $install_prefix ]] ||
	fail "created install prefix changed identity"
[[ $(stat -c '%u:%g:%a' "$install_prefix") == 0:0:755 ]] ||
	fail "created install prefix is not root:root 0755"
as_user test ! -w "$install_prefix" || fail "selected user can write created install prefix"
cleanup_install_prefix=$install_prefix
cmake --install "$build_dir" >/dev/null

validate_installed_path "$helper_dir/howdy-auth-helper" file || fail "unsafe installed helper"
helper=$validated_installed_path
validate_installed_path "$pam_dir/pam_howdy.so" file || fail "unsafe installed PAM module"
module=$validated_installed_path
validate_installed_path "$config_dir/config.ini" file || fail "unsafe installed config"
config=$validated_installed_path
validate_installed_path "$user_models_dir" directory || fail "unsafe installed user-model directory"
models=$validated_installed_path
helper_dir=${helper%/*}
pam_dir=${module%/*}
config_dir=${config%/*}
user_models_dir=$models
[[ $(stat -c '%u:%g:%a' "$helper") == 0:0:4755 ]] || fail "helper is not root:root 4755"
[[ $(stat -c '%u:%g:%a' "$config_dir") == 0:0:750 ]] || fail "config directory is not root:root 0750"
[[ $(stat -c '%u:%g:%a' "$config") == 0:0:640 ]] || fail "config is not root:root 0640"
[[ $(stat -c '%u:%g:%a' "$models") == 0:0:750 ]] || fail "models directory is not root:root 0750"

parent=$helper_dir
while true; do
	as_user test ! -w "$parent" || fail "helper parent writable by selected user: $parent"
	[[ $parent == / ]] && break
	parent=${parent%/*}; [[ -n $parent ]] || parent=/
done
mount_options=$(findmnt -no OPTIONS --target "$helper") || skip "cannot inspect install filesystem"
[[ ,$mount_options, != *,nosuid,* ]] || skip "isolated install filesystem is mounted nosuid"
paths_header=$build_dir/generated/paths.hpp
[[ -f $paths_header ]] || fail "generated compiled-path header missing"
grep -Fx "inline constexpr auto kConfiguredConfigPath = \"$config\";" "$paths_header" >/dev/null ||
	fail "compiled config path mismatch"
grep -Fx "inline constexpr auto kConfiguredUserModelsDir = \"$models\";" "$paths_header" >/dev/null ||
	fail "compiled models path mismatch"
grep -Fx "inline constexpr auto kAuthHelperPath = \"$helper\";" "$paths_header" >/dev/null ||
	fail "compiled helper path mismatch"

python3 - "$config" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1]); text = p.read_text()
for key, value in {"disabled": "true", "abort_if_ssh": "false", "abort_if_lid_closed": "false"}.items():
    lines = text.splitlines()
    hits = [i for i, line in enumerate(lines) if line.startswith(f"{key} = ")]
    if len(hits) != 1: raise SystemExit(f"expected one {key} setting")
    lines[hits[0]] = f"{key} = {value}"; text = "\n".join(lines) + "\n"
p.write_text(text)
PY
chown 0:0 "$config"; chmod 0640 "$config"
cp --preserve=mode,ownership,timestamps "$config" "$source_snapshot"
as_user test ! -r "$config" || fail "protected source config is readable by selected user"

if [[ -e /etc/howdy ]]; then
	[[ -d /etc/howdy && ! -L /etc/howdy ]] || skip "host /etc/howdy cannot be masked safely"
	masked_config_dir=$(mktemp -d -p "$install_prefix" 'masked host config.XXXXXX')
	chmod 0755 "$masked_config_dir"
	mount --bind "$masked_config_dir" /etc/howdy || skip "cannot mask host Howdy config"
	config_masked=true
fi

probe_error=$(mktemp)
if as_user "$helper" prepare '../invalid-e2e-user' >/dev/null 2>"$probe_error"; then
	rm -f "$probe_error"; fail "invalid username unexpectedly succeeded"
fi
if grep -F "must be installed setuid root" "$probe_error" >/dev/null; then
	rm -f "$probe_error"; skip "setuid execution blocked by environment"
fi
grep -F "Invalid user name" "$probe_error" >/dev/null || {
	cat "$probe_error" >&2
	rm -f "$probe_error"
	fail "setuid preflight did not reach username validation"
}
rm -f "$probe_error"
if as_user "$helper" prepare root >/dev/null 2>&1; then
	fail "helper prepared files for different username"
fi

snapshot_runtime_dirs "$runtime_before"
if ! as_user "$helper" prepare "$selected_user" >"$helper_output_file"; then
	fail "helper prepare failed"
fi
if [[ ${HOWDY_E2E_MALFORM_PROTOCOL:-0} == 1 ]]; then
	printf 'UNKNOWN_KEY=injected\n' >>"$helper_output_file"
fi
mapfile -t helper_output <"$helper_output_file"
[[ ${#helper_output[@]} -eq 2 ]] || fail "helper protocol returned wrong line count"
declare -A protocol=()
for line in "${helper_output[@]}"; do
	[[ $line == *=* ]] || fail "helper protocol line lacks separator"
	key=${line%%=*}; value=${line#*=}
	[[ $key == CONFIG_PATH || $key == USER_MODELS_DIR ]] || fail "helper protocol returned unknown key"
	[[ -z ${protocol[$key]+set} ]] || fail "helper protocol returned duplicate key"
	[[ $value == /* ]] || fail "helper protocol returned relative path"
	protocol[$key]=$value
done
[[ -n ${protocol[CONFIG_PATH]:-} && -n ${protocol[USER_MODELS_DIR]:-} ]] || fail "helper protocol omitted key"
staged_config=${protocol[CONFIG_PATH]}; staged_models=${protocol[USER_MODELS_DIR]}
prepared_candidate=${staged_config%/config.ini}
[[ $prepared_candidate != "$staged_config" ]] || fail "staged config has unexpected filename"
[[ -d $prepared_candidate && ! -L $prepared_candidate ]] || fail "runtime root is not real directory"
prepared_canonical=$(realpath -e -- "$prepared_candidate") || fail "cannot canonicalize runtime root"
[[ $prepared_candidate == "$prepared_canonical" ]] || fail "runtime root contains path traversal"
prepared_parent=$(realpath -e -- "$prepared_canonical/..") || fail "cannot canonicalize runtime parent"
[[ $prepared_parent == /run/howdy ]] || fail "runtime root has unexpected parent"
prepared_basename=${prepared_canonical##*/}
[[ $prepared_basename == pam-"$selected_uid"-* &&
	$prepared_basename != pam-"$selected_uid"- ]] || fail "runtime root has unexpected basename"
if grep -Fx "$prepared_basename" "$runtime_before" >/dev/null; then
	fail "helper reused pre-existing runtime root"
fi
prepared_root=$prepared_canonical
[[ $staged_models == "$prepared_root/models" ]] || fail "staged paths do not share runtime root"
[[ $(stat -c '%u:%g' "$prepared_root") == 0:0 ]] || fail "runtime directory is not root-owned"
[[ $(stat -c '%u:%g' /run/howdy) == 0:0 ]] || fail "runtime root is not root-owned"
runtime_mode=$(stat -c '%a' /run/howdy)
(( (8#$runtime_mode & 0022) == 0 )) || fail "runtime root is group/world writable"
if ! cmp -s "$config" "$staged_config"; then
	sha256sum "$config" "$staged_config" >&2
	diff -u "$config" "$staged_config" >&2 || true
	fail "staged config differs from source"
fi

expected_dir_acl=$(printf 'user::r-x\nuser:%s:r-x\ngroup::---\nmask::r-x\nother::---' "$selected_user")
[[ $(getfacl -cpE "$prepared_root") == "$expected_dir_acl" ]] || fail "staged directory ACL mismatch"
expected_file_acl=$(printf 'user::r--\nuser:%s:r--\ngroup::---\nmask::r--\nother::---' "$selected_user")
[[ $(getfacl -cpE "$staged_config") == "$expected_file_acl" ]] || fail "staged config ACL mismatch"
expected_hash=$(sha256sum "$config" | awk '{ print $1 }')
staged_user_hash=$(as_user sha256sum "$staged_config" | awk '{ print $1 }') ||
	fail "selected user cannot read staged config"
[[ $staged_user_hash == "$expected_hash" ]] || fail "selected user read unexpected staged data"

other_record=$(getent passwd | awk -F: -v uid="$selected_uid" '$3 > 0 && $3 != uid { print; exit }')
if [[ -n $other_record ]]; then
	IFS=: read -r _ _ other_uid other_gid _ _ _ <<<"$other_record"
	if getent group "$other_gid" >/dev/null; then
		setpriv --reuid "$other_uid" --regid "$other_gid" --init-groups test ! -r "$staged_config" ||
			fail "another account can read staged config"
		printf 'Other-user ACL isolation: UID %s\n' "$other_uid"
	fi
fi

as_user "$helper" cleanup "$prepared_root"
[[ ! -e $prepared_root ]] || fail "direct helper cleanup left runtime directory"
prepared_root=
cmp -s "$config" "$source_snapshot" || fail "direct helper changed source config"

snapshot_runtime_dirs "$runtime_before"
compare_before=$(pgrep -x howdy-compare 2>/dev/null | sort || true)
service_dir=$(mktemp -d -p "$install_prefix" 'pam service-ทดสอบ.XXXXXX')
chmod 0755 "$service_dir"
printf 'auth required %s workaround=off\n' "$module" >"$service_dir/howdy-e2e"
chmod 0644 "$service_dir/howdy-e2e"
as_user "$driver" howdy-e2e "$selected_user" "$service_dir"
snapshot_runtime_dirs "$runtime_after"
cmp -s "$runtime_before" "$runtime_after" || fail "PAM flow changed runtime directory set"
compare_after=$(pgrep -x howdy-compare 2>/dev/null | sort || true)
[[ $compare_after == "$compare_before" ]] || fail "PAM flow changed compare process set"
cmp -s "$config" "$source_snapshot" || fail "PAM flow changed source config"
[[ $(stat -c '%u:%g:%a' "$config_dir") == 0:0:750 ]] || fail "PAM changed config directory"
[[ $(stat -c '%u:%g:%a' "$config") == 0:0:640 ]] || fail "PAM changed config permissions"

printf 'Installed module: %s\nInstalled helper: %s\n' "$module" "$helper"
printf 'Selected account: %s (UID %s)\n' "$selected_user" "$selected_uid"
printf 'Direct helper: prepare, ACL verification, cleanup passed\n'
printf 'PAM result: PAM_AUTHINFO_UNAVAIL\nRuntime cleanup: passed\n'
