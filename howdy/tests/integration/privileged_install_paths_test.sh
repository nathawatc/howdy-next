#!/bin/sh

set -eu

source_dir=$1
test_root=$2
prefix=/opt/howdy-installed-pam-e2e-configure-test

cleanup() {
	rm -rf -- "$test_root"
}
trap cleanup 0 1 2 3 15

configure_case() {
	name=$1
	shift
	cmake -S "$source_dir" -B "$test_root/$name" \
		-DBUILD_TESTING=ON \
		-DHOWDY_ENABLE_PRIVILEGED_TESTS=ON \
		-DCMAKE_INSTALL_PREFIX="$prefix" \
		"$@"
}

reject_destination() {
	name=$1
	variable=$2
	value=$3
	log=$test_root/$name.log
	if configure_case "$name" "-D${variable}=${value}" >"$log" 2>&1; then
		echo "Escaped install destination unexpectedly configured: ${variable}=${value}" >&2
		exit 1
	fi
	grep -F "Privileged-test path escapes isolated prefix: $value" "$log" >/dev/null || {
		echo "Escaped install destination omitted isolation diagnostic: ${variable}=${value}" >&2
		exit 1
	}
}

[ ! -e "$test_root" ] || {
	echo "Configure-path test root already exists: $test_root" >&2
	exit 1
}
mkdir -m 0700 -- "$test_root"

reject_destination bindir CMAKE_INSTALL_BINDIR /usr/bin
reject_destination datadir CMAKE_INSTALL_DATADIR /usr/share
reject_destination localedir CMAKE_INSTALL_LOCALEDIR /usr/share/locale
reject_destination mandir CMAKE_INSTALL_MANDIR /usr/share/man

configure_case accepted \
	-DCMAKE_INSTALL_BINDIR=bin \
	-DCMAKE_INSTALL_DATADIR=share \
	-DCMAKE_INSTALL_LOCALEDIR=share/locale \
	-DCMAKE_INSTALL_MANDIR=share/man >/dev/null
