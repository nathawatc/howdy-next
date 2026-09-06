#!/usr/bin/env bash
set -euo pipefail

skip() { printf 'SKIP: %s\n' "$1"; exit 77; }
fail() { printf 'FAIL: %s\n' "$1" >&2; exit 1; }
as_user() { setpriv --reuid "$selected_uid" --regid "$selected_gid" --init-groups "$@"; }
assert_locked() {
	local path=$1 message=$2 status
	set +e
	flock -n "$path" true
	status=$?
	set -e
	[[ $status -eq 1 ]] || fail "$message: flock exited $status"
}
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

[[ $# -eq 8 ]] || fail "expected eight path arguments"
build_dir=$1 raw_install_prefix=$2 raw_pam_dir=$3 raw_helper_dir=$4 raw_config_dir=$5
raw_user_models_dir=$6 driver=$7 runtime_driver=$8
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
[[ -n $selected_user ]] || skip "set HOWDY_E2E_USER or use a wrapper that supplies valid SUDO_USER"
passwd_record=$(getent passwd "$selected_user") || skip "selected account does not exist"
IFS=: read -r account_name _ selected_uid selected_gid _ _ _ <<<"$passwd_record"
[[ $account_name == "$selected_user" ]] || fail "selected account name mismatch"
[[ $selected_uid =~ ^[0-9]+$ && $selected_uid -gt 0 ]] || skip "selected account must be non-root"
[[ $selected_gid =~ ^[0-9]+$ ]] || skip "selected account has invalid primary GID"
getent group "$selected_gid" >/dev/null || skip "selected account primary group does not exist"
for command in setpriv findmnt flock mount umount realpath cmake stat getfacl; do
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
service_dir= masked_config_dir= cleanup_install_prefix= config_masked=false
runtime_isolated=false helper=
a_pid= b_pid= c_pid= a_write_fd= b_write_fd= c_write_fd=
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
			printf 'release\n' >&"$fd" 2>/dev/null || true
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

for setting in \
	"disabled true" \
	"abort_if_ssh false" \
	"abort_if_lid_closed false"; do
	key=${setting%% *}
	value=${setting#* }
	count=$(grep -c "^${key} = " "$config" || true)
	[[ $count -eq 1 ]] || fail "expected one $key setting"
	sed -i "s/^${key} = .*/${key} = ${value}/" "$config"
done
chown 0:0 "$config"; chmod 0640 "$config"
source_model=$user_models_dir/$selected_user.dat
write_source_model() {
	local generation=$1 first=$2 second=$3
	printf '[{"id":0,"time":%s,"label":"e2e","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[%s,%s]]}]\n' \
		"$generation" "$first" "$second" >"$source_model"
}
write_source_model 1 0.1 0.2
chown 0:0 "$source_model"
chmod 0600 "$source_model"
canonical_source_model=$(realpath -e -- "$source_model") || fail "cannot canonicalize source model"
[[ $canonical_source_model == "$source_model" ]] || fail "source model path changed identity"
source_model_metadata=$(stat -c '%u:%g:%a:%h:%d:%i' "$source_model")
cp -- "$source_model" "$source_model_snapshot"
cp --preserve=mode,ownership,timestamps "$config" "$source_snapshot"
as_user test ! -r "$config" || fail "protected source config is readable by selected user"
as_user test ! -r "$source_model" || fail "protected source model is readable by selected user"

unrelated_uid= unrelated_gid=
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

probe_error=$(mktemp)
if as_user "$helper" prepare "$selected_user" 3>&- >/dev/null 2>"$probe_error"; then
	rm -f "$probe_error"
	fail "direct helper prepare succeeded without descriptor 3"
fi
expected_lease_error="howdy-auth-helper requires an inherited lease socket on descriptor 3"
[[ $(<"$probe_error") == "$expected_lease_error" ]] || {
	cat "$probe_error" >&2
	rm -f "$probe_error"
	fail "direct helper missing-fd error mismatch"
}
rm -f "$probe_error"

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
assert_locked "$gen_a.lock" "first active lease permits exclusive lock"

write_source_model 2 0.3 0.4
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
assert_locked "$gen_a.lock" "first lease released while active"
assert_locked "$gen_b.lock" "second active lease permits exclusive lock"

write_source_model 3 0.5 0.6
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
flock -n "$gen_a.lock" true || fail "released first lease remains locked"

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
assert_locked "$gen_b.lock" "second lease released while active"
assert_locked "$gen_c.lock" "reused active lease permits exclusive lock"

[[ $(stat -c '%u:%g:%a' /run/howdy) == 0:0:711 ]] || fail "runtime root metadata mismatch"
printf -v expected_directory_acl 'user::rwx\nuser:%s:r-x\ngroup::---\nmask::r-x\nother::---' "$selected_uid"
printf -v expected_file_acl 'user::rw-\nuser:%s:r--\ngroup::---\nmask::r--\nother::---' "$selected_uid"
assert_acl() {
	local path=$1 expected=$2 actual
	actual=$(getfacl -cpn -- "$path") || fail "cannot inspect ACL: $path"
	[[ $actual == "$expected" ]] || fail "ACL mismatch: $path"
}
validate_generation() {
	local generation=$1 lock=$1.lock models_dir=$1/models visible=$1/models/$selected_user.dat
	local backing=$1/.model_backing
	[[ $(stat -c '%u:%g:%a' "$generation") == 0:0:750 ]] || fail "runtime generation metadata mismatch: $generation"
	[[ $(stat -c '%u:%g:%a' "$models_dir") == 0:0:750 ]] || fail "staged models directory metadata mismatch: $models_dir"
	[[ $(stat -c '%u:%g:%a:%h' "$generation/config.ini") == 0:0:640:1 ]] || fail "staged config metadata mismatch: $generation"
	[[ $(stat -c '%u:%g:%a:%h' "$lock") == 0:0:600:1 ]] || fail "runtime lock metadata mismatch: $lock"
	[[ $(stat -c '%u:%g:%a:%h' "$backing") == 0:0:640:2 ]] || fail "model backing metadata mismatch: $backing"
	[[ $(stat -c '%u:%g:%a:%h' "$visible") == 0:0:640:2 ]] || fail "visible model metadata mismatch: $visible"
	[[ $(stat -c '%d:%i' "$backing") == "$(stat -c '%d:%i' "$visible")" ]] || fail "model backing and visible inode differ"
	assert_acl "$generation" "$expected_directory_acl"
	assert_acl "$models_dir" "$expected_directory_acl"
	assert_acl "$generation/config.ini" "$expected_file_acl"
	assert_acl "$visible" "$expected_file_acl"
	assert_acl "$backing" "$expected_file_acl"
	as_user cat "$generation/config.ini" >/dev/null || fail "selected user cannot read staged config"
	as_user cat "$visible" >/dev/null || fail "selected user cannot read staged model"
	if setpriv --reuid "$unrelated_uid" --regid "$unrelated_gid" --clear-groups cat "$generation/config.ini" >/dev/null 2>&1; then
		fail "unrelated user can read staged config"
	fi
	if setpriv --reuid "$unrelated_uid" --regid "$unrelated_gid" --clear-groups cat "$visible" >/dev/null 2>&1; then
		fail "unrelated user can read staged model"
	fi
}
validate_generation "$gen_b"
validate_generation "$gen_c"

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
flock -n "$gen_b.lock" true || fail "released second lease remains locked"
flock -n "$gen_c.lock" true || fail "released reused lease remains locked"

cat "$source_model_snapshot" >"$source_model"
[[ $(stat -c '%u:%g:%a:%h:%d:%i' "$source_model") == "$source_model_metadata" ]] ||
	fail "restored source model metadata or inode changed"
cmp -s "$source_model" "$source_model_snapshot" || fail "source model content was not restored"
as_user test ! -r "$source_model" || fail "restored source model is readable by selected user"

service_dir=$(mktemp -d -p "$install_prefix" 'pam service-ทดสอบ.XXXXXX')
chmod 0755 "$service_dir"
printf 'auth required %s workaround=off\n' "$module" >"$service_dir/howdy-e2e"
chmod 0644 "$service_dir/howdy-e2e"
as_user "$driver" howdy-e2e "$selected_user" "$service_dir"
flock -n "$expected_gen0.lock" true || fail "PAM flow retained gen000 lease"
flock -n "$expected_gen1.lock" true || fail "PAM flow retained gen001 lease"
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
printf 'Canonical source model: restored, unchanged, protected\n'
printf 'Direct helper missing-fd rejection: passed\n'
printf 'PAM-only result: PAM_AUTHINFO_UNAVAIL\nRuntime cleanup: passed\n'
