#!/usr/bin/env bash
set -euo pipefail

skip() {
	printf 'SKIP: %s\n' "$1"
	exit 77
}
fail() {
	printf 'FAIL: %s\n' "$1" >&2
	exit 1
}
as_user() { setpriv --reuid "$selected_uid" --regid "$selected_gid" --init-groups "$@"; }
validate_opt_directory() {
	local canonical mode
	[[ -d /opt && ! -L /opt ]] || return 1
	canonical=$(realpath -e -- /opt) || return 1
	[[ $canonical == /opt ]] || return 1
	[[ $(stat -c '%u' /opt) == 0 ]] || return 1
	mode=$(stat -c '%a' /opt) || return 1
	(((8#$mode & 0022) == 0)) || return 1
	as_user test ! -w /opt || return 1
}
[[ $# -eq 11 ]] || fail "expected validators and nine path arguments"
path_validator=$1 state_verifier=$2 build_dir=$3 raw_install_prefix=$4 raw_pam_dir=$5
raw_helper_dir=$6 raw_config_dir=$7 raw_user_models_dir=$8 driver=$9 runtime_driver=${10}
compare_probe=${11}
[[ -f $path_validator && -x $path_validator ]] || fail "path validator is unavailable"
[[ -f $state_verifier && -x $state_verifier ]] || fail "state verifier is unavailable"
install_prefix=$("$path_validator" --validate-prefix "$raw_install_prefix") || fail "unsafe isolated install prefix"
pam_dir=$("$path_validator" --validate-subdir "$install_prefix" "$raw_pam_dir") || fail "unsafe PAM directory"
helper_dir=$("$path_validator" --validate-subdir "$install_prefix" "$raw_helper_dir") || fail "unsafe helper directory"
config_dir=$("$path_validator" --validate-subdir "$install_prefix" "$raw_config_dir") || fail "unsafe config directory"
user_models_dir=$("$path_validator" --validate-subdir "$install_prefix" "$raw_user_models_dir") || fail "unsafe user-model directory"
unset DESTDIR
unset CMAKE_INSTALL_MODE
[[ $EUID -eq 0 ]] || skip "root privileges required"
if [[ ${HOWDY_E2E_MOUNT_NAMESPACE:-0} != 1 ]]; then
	command -v unshare > /dev/null || skip "unshare unavailable"
	unshare --mount --propagation private true 2> /dev/null || skip "private mount namespace unavailable"
	exec unshare --mount --propagation private env HOWDY_E2E_MOUNT_NAMESPACE=1 bash "$0" "$@"
fi
selected_user=${HOWDY_E2E_USER:-${SUDO_USER:-}}
[[ -n $selected_user ]] || skip "set HOWDY_E2E_USER or use a wrapper that supplies valid SUDO_USER"
passwd_record=$(getent passwd "$selected_user") || skip "selected account does not exist"
IFS=: read -r account_name _ selected_uid selected_gid _ _ _ <<< "$passwd_record"
[[ $account_name == "$selected_user" ]] || fail "selected account name mismatch"
[[ $selected_uid =~ ^[0-9]+$ && $selected_uid -gt 0 ]] || skip "selected account must be non-root"
[[ $selected_gid =~ ^[0-9]+$ ]] || skip "selected account has invalid primary GID"
getent group "$selected_gid" > /dev/null || skip "selected account primary group does not exist"
for command in setpriv findmnt mount umount realpath cmake stat cmp; do
	command -v "$command" > /dev/null || skip "$command unavailable"
done
validate_opt_directory || fail "unsafe /opt directory"

source_snapshot=$(mktemp)
source_model_snapshot=$(mktemp)
service_dir="" masked_config_dir="" cleanup_install_prefix="" config_masked=false
runtime_isolated=false helper=""
cleanup() {
	status=$?
	trap - EXIT HUP INT TERM
	trap '' PIPE
	if $config_masked; then umount /etc/howdy > /dev/null 2>&1 || true; fi
	if $runtime_isolated; then umount /run > /dev/null 2>&1 || true; fi
	[[ -z ${service_dir:-} ]] || rm -rf -- "$service_dir"
	if [[ -n $cleanup_install_prefix && -d $cleanup_install_prefix &&
		! -L $cleanup_install_prefix &&
		$(realpath -e -- "$cleanup_install_prefix") == "$cleanup_install_prefix" ]]; then
		rm -rf -- "$cleanup_install_prefix"
	fi
	rm -f -- "$source_snapshot" "$source_model_snapshot"
	exit "$status"
}
trap cleanup EXIT
trap 'exit 128' HUP INT TERM

# An empty runtime root, so the test can show that nothing is staged there any more.
mount -t tmpfs -o mode=0755,nodev,nosuid tmpfs /run || skip "cannot isolate runtime root"
runtime_isolated=true
[[ ! -e /run/howdy ]] || fail "isolated runtime root is not initially absent"

[[ ! -e $install_prefix ]] || fail "isolated install prefix already exists"
mkdir -m 0755 -- "$install_prefix"
chown 0:0 "$install_prefix"
chmod 0755 "$install_prefix"
created_install_prefix=$(realpath -e -- "$install_prefix") || fail "cannot canonicalize install prefix"
[[ $created_install_prefix == "$install_prefix" && -d $install_prefix && ! -L $install_prefix ]] \
	|| fail "created install prefix changed identity"
[[ $(stat -c '%u:%g:%a' "$install_prefix") == 0:0:755 ]] \
	|| fail "created install prefix is not root:root 0755"
as_user test ! -w "$install_prefix" || fail "selected user can write created install prefix"
cleanup_install_prefix=$install_prefix
cmake --install "$build_dir" > /dev/null

helper=$("$path_validator" --validate-installed "$install_prefix" file "$helper_dir/howdy-auth-helper") || fail "unsafe installed helper"
compare=$("$path_validator" --validate-installed "$install_prefix" file "$helper_dir/howdy-compare") || fail "unsafe installed compare"
module=$("$path_validator" --validate-installed "$install_prefix" file "$pam_dir/pam_howdy.so") || fail "unsafe installed PAM module"
config=$("$path_validator" --validate-installed "$install_prefix" file "$config_dir/config.ini") || fail "unsafe installed config"
models=$("$path_validator" --validate-installed "$install_prefix" directory "$user_models_dir") || fail "unsafe installed user-model directory"
helper_dir=${helper%/*}
pam_dir=${module%/*}
config_dir=${config%/*}
user_models_dir=$models
"$state_verifier" --installed "$helper" "$compare" "$config_dir" "$config" "$models" \
	"$selected_uid" "$selected_gid" || fail "installed file verification failed"
mount_options=$(findmnt -no OPTIONS --target "$helper") || skip "cannot inspect install filesystem"
[[ ,$mount_options, != *,nosuid,* ]] || skip "isolated install filesystem is mounted nosuid"
"$state_verifier" --configure "$build_dir/generated/paths.hpp" "$config" "$models" "$helper" "$compare" \
	|| fail "compiled paths or config fixture verification failed"
chown 0:0 "$config"
chmod 0640 "$config"
source_model=$user_models_dir/$selected_user.dat
"$state_verifier" --write-model "$source_model" 1 0.1 0.2 || fail "cannot create source model"
chown 0:0 "$source_model"
chmod 0600 "$source_model"
canonical_source_model=$(realpath -e -- "$source_model") || fail "cannot canonicalize source model"
[[ $canonical_source_model == "$source_model" ]] || fail "source model path changed identity"
source_model_metadata=$(stat -c '%u:%g:%a:%h:%d:%i' "$source_model")
cp -- "$source_model" "$source_model_snapshot"
cp --preserve=mode,ownership,timestamps "$config" "$source_snapshot"
as_user test ! -r "$config" || fail "protected source config is readable by selected user"
as_user test ! -r "$source_model" || fail "protected source model is readable by selected user"

if [[ -e /etc/howdy ]]; then
	[[ -d /etc/howdy && ! -L /etc/howdy ]] || skip "host /etc/howdy cannot be masked safely"
	masked_config_dir=$(mktemp -d -p "$install_prefix" 'masked host config.XXXXXX')
	chmod 0755 "$masked_config_dir"
	mount --bind "$masked_config_dir" /etc/howdy || skip "cannot mask host Howdy config"
	config_masked=true
fi

probe_status=0
"$state_verifier" --helper-probes "$helper" "$selected_user" "$selected_uid" "$selected_gid" \
	"$config" "$source_model" || probe_status=$?
if [[ $probe_status -eq 77 ]]; then skip "setuid execution blocked by environment"; fi
[[ $probe_status -eq 0 ]] || fail "direct helper probes failed"

# The production runtime session: through the installed helper as the selected user, and
# in-process as root.
open_runtime() {
	local expected=$1
	as_user "$runtime_driver" --open "$selected_user" "$config" "$user_models_dir" "$expected" \
		> /dev/null || fail "helper runtime session did not report a $expected model"
}
open_runtime ready
"$runtime_driver" --open "$selected_user" "$config" "$user_models_dir" ready > /dev/null \
	|| fail "in-process runtime session did not report a ready model"
mv -- "$source_model" "$source_model.absent"
open_runtime missing
mv -- "$source_model.absent" "$source_model"
chmod 0606 "$source_model"
open_runtime invalid
chmod 0600 "$source_model"

service_dir=$(mktemp -d -p "$install_prefix" 'pam service-ทดสอบ.XXXXXX')
chmod 0755 "$service_dir"
printf 'auth required %s workaround=off\n' "$module" > "$service_dir/howdy-e2e"
chmod 0644 "$service_dir/howdy-e2e"
sed -i 's/^disabled = true$/disabled = false/' "$config"
mv -- "$source_model" "$source_model.absent"
as_user "$driver" howdy-e2e "$selected_user" "$service_dir" unavailable
mv -- "$source_model.absent" "$source_model"
cat "$source_snapshot" > "$config"

# Exercise the installed PAM, setuid helper, descriptor passing, and compiled compare path
# with a controlled result independent of camera hardware and ONNX model assets.
[[ -f $compare_probe && -x $compare_probe ]] || fail "compare probe is unavailable"
mv -- "$compare" "$compare.production"
install -o 0 -g 0 -m 0755 -- "$compare_probe" "$compare"
sed -i 's/^disabled = true$/disabled = false/' "$config"
if ! success_output=$(as_user "$driver" howdy-e2e "$selected_user" "$service_dir" success 2>&1); then
	printf '%s\n' "$success_output" >&2
	fail "installed PAM success path failed"
fi
[[ $success_output == $'HOWDY_E2E_COMPARE_PROBE_OK\nPAM_SUCCESS' ]] \
	|| fail "installed compare execution or PAM success marker missing: $success_output"
cat "$source_snapshot" > "$config"
rm -- "$compare"
mv -- "$compare.production" "$compare"
[[ $(stat -c '%u:%g:%a' "$compare") == 0:0:755 ]] || fail "restored compare metadata changed"

as_user "$driver" howdy-e2e "$selected_user" "$service_dir" unavailable
[[ ! -e /run/howdy ]] || fail "runtime files were staged under /run/howdy"
cmp -s "$config" "$source_snapshot" || fail "runtime flow changed source config"
cmp -s "$source_model" "$source_model_snapshot" || fail "runtime flow changed source model"
[[ $(stat -c '%u:%g:%a:%h:%d:%i' "$source_model") == "$source_model_metadata" ]] \
	|| fail "runtime flow changed source model metadata or inode"
as_user test ! -r "$source_model" || fail "runtime flow exposed source model to selected user"
[[ $(stat -c '%u:%g:%a' "$config_dir") == 0:0:750 ]] || fail "runtime flow changed config directory"
[[ $(stat -c '%u:%g:%a' "$config") == 0:0:640 ]] || fail "runtime flow changed config permissions"

printf 'Installed module: %s\nInstalled helper: %s\n' "$module" "$helper"
printf 'Selected account: %s (UID %s)\n' "$selected_user" "$selected_uid"
printf 'Helper open: guards, one ready message, read-only descriptors matching the sources\n'
printf 'Runtime session: helper and in-process opens; ready, missing, and invalid models\n'
printf 'Canonical source model: unchanged, protected; nothing staged under /run/howdy\n'
printf 'Installed PAM compare result: PAM_SUCCESS via validated compare probe\n'
printf 'PAM unavailable results: passed\n'
