#!/usr/bin/env bash
set -euo pipefail

skip() { printf 'SKIP: %s\n' "$1"; exit 77; }
fail() { printf 'FAIL: %s\n' "$1" >&2; exit 1; }
as_user() { setpriv --reuid "$selected_uid" --regid "$selected_gid" --init-groups "$@"; }
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
[[ -n $selected_user ]] || skip "set HOWDY_E2E_USER or use a wrapper that supplies valid SUDO_USER"
passwd_record=$(getent passwd "$selected_user") || skip "selected account does not exist"
IFS=: read -r account_name _ selected_uid selected_gid _ _ _ <<<"$passwd_record"
[[ $account_name == "$selected_user" ]] || fail "selected account name mismatch"
[[ $selected_uid =~ ^[0-9]+$ && $selected_uid -gt 0 ]] || skip "selected account must be non-root"
[[ $selected_gid =~ ^[0-9]+$ ]] || skip "selected account has invalid primary GID"
getent group "$selected_gid" >/dev/null || skip "selected account primary group does not exist"
for command in setpriv findmnt mount umount realpath cmake stat setfacl; do
	command -v "$command" >/dev/null || skip "$command unavailable"
done
validate_opt_directory || fail "unsafe /opt directory"

runtime_before=$(mktemp)
runtime_after=$(mktemp)
source_snapshot=$(mktemp)
source_model_snapshot=$(mktemp)
model_b_snapshot=$(mktemp)
model_c_snapshot=$(mktemp)
third_error=$(mktemp)
service_dir="" masked_config_dir="" cleanup_install_prefix="" config_masked=false
runtime_isolated=false helper=""
a_pid="" b_pid="" c_pid="" a_write_fd="" b_write_fd="" c_write_fd=""
cleanup() {
	status=$?
	trap - EXIT HUP INT TERM
	trap '' PIPE
	for holder in a b c; do
		case $holder in
			a) pid=${a_pid:-}; fd=${a_write_fd:-} ;;
			b) pid=${b_pid:-}; fd=${b_write_fd:-} ;;
			c) pid=${c_pid:-}; fd=${c_write_fd:-} ;;
		esac
		if [[ -n $fd ]]; then
			eval "printf 'release\\n' >&${fd} 2>/dev/null" || true
			eval "exec ${fd}>&-"
		fi
		if [[ -n $pid ]]; then
			kill "$pid" >/dev/null 2>&1 || true
			wait "$pid" >/dev/null 2>&1 || true
		fi
	done
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
	rm -f -- "$runtime_before" "$runtime_after" "$source_snapshot" \
		"$source_model_snapshot" "$model_b_snapshot" "$model_c_snapshot" "$third_error"
	exit "$status"
}
trap cleanup EXIT
trap 'exit 128' HUP INT TERM

mount -t tmpfs -o mode=0755,nodev,nosuid tmpfs /run || skip "cannot isolate runtime root"
runtime_isolated=true
acl_status=0
"$state_verifier" --acl-capability /run "$selected_uid" || acl_status=$?
if [[ $acl_status -eq 77 ]]; then skip "isolated runtime filesystem lacks POSIX ACL support"; fi
[[ $acl_status -eq 0 ]] || fail "isolated runtime ACL capability check failed"
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
"$state_verifier" --configure "$build_dir/generated/paths.hpp" "$config" "$models" "$helper" "$compare" ||
	fail "compiled paths or config fixture verification failed"
chown 0:0 "$config"; chmod 0640 "$config"
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

unrelated_uid="" unrelated_gid=""
while IFS=: read -r _ _ candidate_uid candidate_gid _; do
	if [[ $candidate_uid =~ ^[0-9]+$ && $candidate_gid =~ ^[0-9]+$ &&
		$candidate_uid -gt 0 && $candidate_uid -ne $selected_uid ]]; then
		unrelated_uid=$candidate_uid
		unrelated_gid=$candidate_gid
		break
	fi
done < <(getent passwd)
[[ -n $unrelated_uid ]] || skip "unrelated non-root account unavailable"
setpriv --reuid "$unrelated_uid" --regid "$unrelated_gid" --clear-groups true ||
	skip "unrelated account credential transition unavailable"

if [[ -e /etc/howdy ]]; then
	[[ -d /etc/howdy && ! -L /etc/howdy ]] || skip "host /etc/howdy cannot be masked safely"
	masked_config_dir=$(mktemp -d -p "$install_prefix" 'masked host config.XXXXXX')
	chmod 0755 "$masked_config_dir"
	mount --bind "$masked_config_dir" /etc/howdy || skip "cannot mask host Howdy config"
	config_masked=true
fi

probe_status=0
"$state_verifier" --helper-probes "$helper" "$selected_user" "$selected_uid" "$selected_gid" || probe_status=$?
if [[ $probe_status -eq 77 ]]; then skip "setuid execution blocked by environment"; fi
[[ $probe_status -eq 0 ]] || fail "direct helper preflight failed"

snapshot_runtime_dirs "$runtime_before"
[[ ! -s $runtime_before ]] || fail "isolated runtime unexpectedly contains selected-user generations"

coproc HOLD_A {
	exec setpriv --reuid "$selected_uid" --regid "$selected_gid" --init-groups \
		"$runtime_driver" --hold "$selected_user" "$config" "$user_models_dir"
}
exec {a_read_fd}<&"${HOLD_A[0]}"
exec {a_write_fd}>&"${HOLD_A[1]}"
a_pid=$HOLD_A_PID
read -r -t 10 gen_a <&"$a_read_fd" || fail "first runtime holder did not become ready"
exec {a_read_fd}<&-
expected_gen0=/run/howdy/pam-$selected_uid-gen000
expected_gen1=/run/howdy/pam-$selected_uid-gen001
[[ $gen_a == "$expected_gen0" ]] || fail "first runtime generation mismatch: $gen_a"
cmp -s "$source_model_snapshot" "$gen_a/models/$selected_user.dat" ||
	fail "first runtime generation content mismatch"
"$state_verifier" --locked "$gen_a.lock" || fail "first active lease permits exclusive lock"

"$state_verifier" --write-model "$source_model" 2 0.3 0.4 || fail "cannot mutate source model"
[[ $(stat -c '%u:%g:%a:%h:%d:%i' "$source_model") == "$source_model_metadata" ]] ||
	fail "first source-model mutation changed security metadata or inode"
cp -- "$source_model" "$model_b_snapshot"
coproc HOLD_B {
	exec setpriv --reuid "$selected_uid" --regid "$selected_gid" --init-groups \
		"$runtime_driver" --hold "$selected_user" "$config" "$user_models_dir"
}
exec {b_read_fd}<&"${HOLD_B[0]}"
exec {b_write_fd}>&"${HOLD_B[1]}"
b_pid=$HOLD_B_PID
read -r -t 10 gen_b <&"$b_read_fd" || fail "second runtime holder did not become ready"
exec {b_read_fd}<&-
[[ $gen_b == "$expected_gen1" ]] || fail "second runtime generation mismatch: $gen_b"
cmp -s "$model_b_snapshot" "$gen_b/models/$selected_user.dat" ||
	fail "second runtime generation did not stage second source content"
cmp -s "$source_model_snapshot" "$gen_a/models/$selected_user.dat" ||
	fail "first active runtime generation changed after source mutation"
"$state_verifier" --locked "$gen_a.lock" || fail "first lease released while active"
"$state_verifier" --locked "$gen_b.lock" || fail "second active lease permits exclusive lock"

"$state_verifier" --write-model "$source_model" 3 0.5 0.6 || fail "cannot mutate source model"
[[ $(stat -c '%u:%g:%a:%h:%d:%i' "$source_model") == "$source_model_metadata" ]] ||
	fail "second source-model mutation changed security metadata or inode"
cp -- "$source_model" "$model_c_snapshot"
set +e
as_user "$runtime_driver" --try "$selected_user" "$config" "$user_models_dir" \
	>/dev/null 2>"$third_error"
third_status=$?
set -e
[[ $third_status -eq 1 ]] || fail "third runtime request did not fail closed: $third_status"
[[ $(<"$third_error") == 'Runtime staging failed' ]] || fail "third runtime failure mismatch"

snapshot_runtime_dirs "$runtime_after"
mapfile -t runtime_names <"$runtime_after"
[[ ${#runtime_names[@]} -eq 2 && ${runtime_names[0]} == "${expected_gen0##*/}" &&
	${runtime_names[1]} == "${expected_gen1##*/}" ]] ||
	fail "runtime generations are not exactly gen000 and gen001"

printf 'release\n' >&"$a_write_fd"
exec {a_write_fd}>&-
wait "$a_pid" || fail "first runtime holder failed during release"
a_pid=
a_write_fd=
"$state_verifier" --unlocked "$gen_a.lock" || fail "released first lease remains locked"

coproc HOLD_C {
	exec setpriv --reuid "$selected_uid" --regid "$selected_gid" --init-groups \
		"$runtime_driver" --hold "$selected_user" "$config" "$user_models_dir"
}
exec {c_read_fd}<&"${HOLD_C[0]}"
exec {c_write_fd}>&"${HOLD_C[1]}"
c_pid=$HOLD_C_PID
read -r -t 10 gen_c <&"$c_read_fd" || fail "reused runtime holder did not become ready"
exec {c_read_fd}<&-
[[ $gen_c == "$gen_a" ]] || fail "released runtime slot was not reused"
cmp -s "$model_c_snapshot" "$gen_c/models/$selected_user.dat" ||
	fail "reused runtime slot did not stage third source content"
cmp -s "$model_b_snapshot" "$gen_b/models/$selected_user.dat" ||
	fail "second active runtime generation changed during slot reuse"
"$state_verifier" --locked "$gen_b.lock" || fail "second lease released while active"
"$state_verifier" --locked "$gen_c.lock" || fail "reused active lease permits exclusive lock"

"$state_verifier" --runtime-root /run/howdy || fail "runtime root metadata mismatch"
"$state_verifier" --generation "$gen_b" "$selected_user" "$selected_uid" "$selected_gid" "$unrelated_uid" "$unrelated_gid" || fail "second generation verification failed"
"$state_verifier" --generation "$gen_c" "$selected_user" "$selected_uid" "$selected_gid" "$unrelated_uid" "$unrelated_gid" || fail "reused generation verification failed"

printf 'release\n' >&"$b_write_fd"
exec {b_write_fd}>&-
wait "$b_pid" || fail "second runtime holder failed during release"
b_pid=
b_write_fd=
printf 'release\n' >&"$c_write_fd"
exec {c_write_fd}>&-
wait "$c_pid" || fail "reused runtime holder failed during release"
c_pid=
c_write_fd=
"$state_verifier" --unlocked "$gen_b.lock" || fail "released second lease remains locked"
"$state_verifier" --unlocked "$gen_c.lock" || fail "released reused lease remains locked"

# Keep the canonical inode for restoration while exercising present-to-absent refresh.
mv -- "$source_model" "$source_model.absent"
coproc HOLD_A {
	exec setpriv --reuid "$selected_uid" --regid "$selected_gid" --init-groups \
		"$runtime_driver" --hold-absent "$selected_user" "$config" "$user_models_dir"
}
exec {a_read_fd}<&"${HOLD_A[0]}"
exec {a_write_fd}>&"${HOLD_A[1]}"
a_pid=$HOLD_A_PID
read -r -t 10 gen_absent <&"$a_read_fd" || fail "absent runtime holder did not become ready"
exec {a_read_fd}<&-
[[ $gen_absent == "$expected_gen0" ]] || fail "absent refresh did not reuse first slot"
backing=$gen_absent/.model_backing
"$state_verifier" --absent "$gen_absent" "$selected_user" "$selected_uid" "$selected_gid" || fail "absent generation verification failed"
"$state_verifier" --locked "$gen_absent.lock" || fail "absent session lost active lease"
inspect_absent() {
	as_user "$runtime_driver" "$1" "$selected_user" "$config" "$gen_absent/models" ||
		fail "absent sentinel inspection failed: $1"
}
inspect_absent --inspect-absent
chown "$selected_uid":0 "$backing"
inspect_absent --inspect-insecure
chown 0:0 "$backing"
chgrp "$selected_gid" "$backing"
if [[ $selected_gid -ne 0 ]]; then inspect_absent --inspect-insecure; fi
chgrp 0 "$backing"
chmod 0640 "$backing"
inspect_absent --inspect-insecure
chmod 0600 "$backing"
# An extended ACL masked to zero still has mode 0600, but is not owner-only.
setfacl -m "u:$selected_uid:r--,m::---" "$backing"
inspect_absent --inspect-insecure
setfacl -b "$backing"
ln "$backing" "$gen_absent/extra-link"
inspect_absent --inspect-insecure
unlink "$gen_absent/extra-link"
mv "$backing" "$backing.saved"
ln -s .model_backing.saved "$backing"
inspect_absent --inspect-insecure
unlink "$backing"
mv "$backing.saved" "$backing"
inspect_absent --inspect-absent
"$state_verifier" --locked "$gen_absent.lock" || fail "sentinel checks released active lease"
service_dir=$(mktemp -d -p "$install_prefix" 'pam service-ทดสอบ.XXXXXX')
chmod 0755 "$service_dir"
printf 'auth required %s workaround=off\n' "$module" >"$service_dir/howdy-e2e"
chmod 0644 "$service_dir/howdy-e2e"
sed -i 's/^disabled = true$/disabled = false/' "$config"
as_user "$driver" howdy-e2e "$selected_user" "$service_dir" unavailable
cat "$source_snapshot" >"$config"
printf 'release\n' >&"$a_write_fd"
exec {a_write_fd}>&-
wait "$a_pid" || fail "absent runtime holder failed during release"
a_pid=
a_write_fd=
"$state_verifier" --unlocked "$gen_absent.lock" || fail "released absent lease remains locked"
mv -- "$source_model.absent" "$source_model"

cat "$source_model_snapshot" >"$source_model"
[[ $(stat -c '%u:%g:%a:%h:%d:%i' "$source_model") == "$source_model_metadata" ]] ||
	fail "restored source model metadata or inode changed"
cmp -s "$source_model" "$source_model_snapshot" || fail "source model content was not restored"
as_user test ! -r "$source_model" || fail "restored source model is readable by selected user"

# Exercise the installed PAM, setuid helper, staged runtime, and compiled compare path
# with a controlled result independent of camera hardware and ONNX model assets.
[[ -f $compare_probe && -x $compare_probe ]] || fail "compare probe is unavailable"
mv -- "$compare" "$compare.production"
install -o 0 -g 0 -m 0755 -- "$compare_probe" "$compare"
sed -i 's/^disabled = true$/disabled = false/' "$config"
if ! success_output=$(as_user "$driver" howdy-e2e "$selected_user" "$service_dir" success 2>&1); then
	printf '%s\n' "$success_output" >&2
	fail "installed PAM success path failed"
fi
[[ $success_output == $'HOWDY_E2E_COMPARE_PROBE_OK\nPAM_SUCCESS' ]] ||
	fail "installed compare execution or PAM success marker missing: $success_output"
cat "$source_snapshot" >"$config"
rm -- "$compare"
mv -- "$compare.production" "$compare"
[[ $(stat -c '%u:%g:%a' "$compare") == 0:0:755 ]] || fail "restored compare metadata changed"

as_user "$driver" howdy-e2e "$selected_user" "$service_dir" unavailable
"$state_verifier" --unlocked "$expected_gen0.lock" || fail "PAM flow retained gen000 lease"
"$state_verifier" --unlocked "$expected_gen1.lock" || fail "PAM flow retained gen001 lease"
cmp -s "$config" "$source_snapshot" || fail "runtime flow changed source config"
cmp -s "$source_model" "$source_model_snapshot" || fail "PAM flow changed restored source model"
[[ $(stat -c '%u:%g:%a:%h:%d:%i' "$source_model") == "$source_model_metadata" ]] ||
	fail "PAM flow changed source model metadata or inode"
as_user test ! -r "$source_model" || fail "PAM flow exposed source model to selected user"
[[ $(stat -c '%u:%g:%a' "$config_dir") == 0:0:750 ]] || fail "runtime flow changed config directory"
[[ $(stat -c '%u:%g:%a' "$config") == 0:0:640 ]] || fail "runtime flow changed config permissions"

printf 'Installed module: %s\nInstalled helper: %s\n' "$module" "$helper"
printf 'Selected account: %s (UID %s)\n' "$selected_user" "$selected_uid"
printf 'Runtime leases: stale two-slot exhaustion and released-slot refresh passed\n'
printf 'Runtime files: three content generations, exact metadata/ACLs, links, load/access passed\n'
printf 'Absent staging: target-user readiness, sentinel rejection, active lease, PAM result passed\n'
printf 'Canonical source model: restored, unchanged, protected\n'
printf 'Direct helper missing-fd rejection: passed\n'
printf 'Installed PAM compare result: PAM_SUCCESS via validated compare probe\n'
printf 'PAM unavailable results and runtime cleanup: passed\n'
