#!/usr/bin/env bash
# Prints shell exports so a CMake build uses the compiler and hardening flags a
# distribution package build would, which is where blind ports break first.
#
#   eval "$(bash ci/common/distro-build-flags.sh dpkg|rpm|clang)"
set -euo pipefail

case "${1:-}" in
	dpkg)
		# dh_auto_configure folds CPPFLAGS into the C++ flags; CMake does not.
		printf 'export CXXFLAGS=%q\n' "$(dpkg-buildflags --get CPPFLAGS) $(dpkg-buildflags --get CXXFLAGS)"
		printf 'export LDFLAGS=%q\n' "$(dpkg-buildflags --get LDFLAGS)"
		;;
	rpm)
		printf 'export CXXFLAGS=%q\n' "$(rpm --eval '%{build_cxxflags}')"
		printf 'export LDFLAGS=%q\n' "$(rpm --eval '%{build_ldflags}')"
		;;
	clang)
		printf 'export CXX=clang++\n'
		;;
	*)
		printf 'usage: %s dpkg|rpm|clang\n' "$0" >&2
		exit 2
		;;
esac
