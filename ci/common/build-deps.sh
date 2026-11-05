#!/usr/bin/env bash
# Builds what a distribution's packages cannot provide, plus the face models,
# into /opt/howdy-deps. Used by the Debian, Ubuntu, and Fedora CI Containerfiles.
#
#   build-deps.sh libs     OpenCV 5, and yyjson when the distro's is too old
#   build-deps.sh models   download and verify the ONNX models
#
# Floors match the root CMakeLists.txt; yyjson, inih, and OpenCV are each built only
# when the distribution's pkg-config version is below the floor. OpenCV flags mirror
# nix/opencv5.nix so every CI environment tests the same OpenCV configuration.
set -euo pipefail

readonly PREFIX=/opt/howdy-deps
readonly OPENCV_VERSION=5.0.0
readonly OPENCV_SHA256=b0528f5a1d379d59d4701cb28c36e22214cc51cf64594e5b56f2d3e6c0233095
readonly YYJSON_VERSION=0.12.0
readonly YYJSON_SHA256=b16246f617b2a136c78d73e5e2647c6f1de1313e46678062985bdcf1f40bb75d
readonly INIH_VERSION=62
readonly INIH_SHA256=9c15fa751bb8093d042dae1b9f125eb45198c32c6704cd5481ccde460d4f8151
readonly OPENCV_MINIMUM=5.0.0
readonly YYJSON_MINIMUM=0.12.0
readonly INIREADER_MINIMUM=59
readonly MANIFEST="${HOWDY_MODEL_MANIFEST:-/tmp/opencv_model_manifest.hpp}"

export PKG_CONFIG_PATH="${PREFIX}/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"

log() {
	printf '==> %s\n' "$*" >&2
}

die() {
	printf 'error: %s\n' "$*" >&2
	exit 1
}

satisfied() {
	pkg-config --atleast-version "$2" "$1"
}

fetch() {
	local url=$1 sha256=$2 output=$3
	curl --fail --silent --show-error --location --retry 3 --output "$output" "$url"
	printf '%s  %s\n' "$sha256" "$output" | sha256sum --check --status \
		|| die "checksum mismatch for $url"
}

# CMake can bake the install prefix into a .pc twice; collapse it.
fix_pkgconfig_prefix() {
	sed -i "s|{exec_prefix}/${PREFIX}|{exec_prefix}|;s|{prefix}/${PREFIX}|{prefix}|" "$1"
}

build_yyjson() {
	local work=$1
	log "building yyjson ${YYJSON_VERSION}"
	fetch "https://github.com/ibireme/yyjson/archive/refs/tags/${YYJSON_VERSION}.tar.gz" \
		"$YYJSON_SHA256" "$work/yyjson.tar.gz"
	tar -xzf "$work/yyjson.tar.gz" -C "$work"
	cmake -S "$work/yyjson-${YYJSON_VERSION}" -B "$work/yyjson-build" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$PREFIX" \
		-DCMAKE_INSTALL_LIBDIR=lib \
		-DBUILD_SHARED_LIBS=ON \
		-DYYJSON_BUILD_TESTS=OFF
	cmake --build "$work/yyjson-build" --parallel "$(nproc)"
	cmake --install "$work/yyjson-build"
	test -s "$PREFIX/lib/pkgconfig/yyjson.pc" || die "yyjson.pc was not installed"
	fix_pkgconfig_prefix "$PREFIX/lib/pkgconfig/yyjson.pc"
	satisfied yyjson "$YYJSON_MINIMUM" || die "built yyjson does not satisfy >= ${YYJSON_MINIMUM}"
}

# Needs meson and ninja in the image. Debian 13 packages inih r59 but its pkg-config
# files report 58, so the INIReader>=59 floor in CMakeLists.txt cannot be met by it.
build_inih() {
	local work=$1
	command -v meson > /dev/null || die "meson is required to build inih"
	log "building inih r${INIH_VERSION}"
	fetch "https://github.com/benhoyt/inih/archive/refs/tags/r${INIH_VERSION}.tar.gz" \
		"$INIH_SHA256" "$work/inih.tar.gz"
	tar -xzf "$work/inih.tar.gz" -C "$work"
	meson setup "$work/inih-build" "$work/inih-r${INIH_VERSION}" \
		--prefix="$PREFIX" \
		--libdir=lib \
		--buildtype=release \
		-Ddefault_library=shared \
		-Dtests=false
	meson compile -C "$work/inih-build"
	meson install -C "$work/inih-build"
	test -s "$PREFIX/lib/pkgconfig/INIReader.pc" || die "INIReader.pc was not installed"
	satisfied INIReader "$INIREADER_MINIMUM" \
		|| die "built inih does not satisfy INIReader >= ${INIREADER_MINIMUM}"
}

build_opencv() {
	local work=$1
	log "building OpenCV ${OPENCV_VERSION}"
	fetch "https://github.com/opencv/opencv/archive/refs/tags/${OPENCV_VERSION}.tar.gz" \
		"$OPENCV_SHA256" "$work/opencv.tar.gz"
	tar -xzf "$work/opencv.tar.gz" -C "$work"
	cmake -S "$work/opencv-${OPENCV_VERSION}" -B "$work/opencv-build" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$PREFIX" \
		-DCMAKE_INSTALL_LIBDIR=lib \
		-DCMAKE_SKIP_BUILD_RPATH=ON \
		-DBUILD_LIST=core,imgproc,imgcodecs,dnn,videoio,highgui,objdetect \
		-DBUILD_SHARED_LIBS=ON \
		-DBUILD_TESTS=OFF \
		-DBUILD_PERF_TESTS=OFF \
		-DBUILD_EXAMPLES=OFF \
		-DBUILD_DOCS=OFF \
		-DBUILD_PACKAGE=OFF \
		-DBUILD_opencv_apps=OFF \
		-DBUILD_opencv_python3=OFF \
		-DOPENCV_GENERATE_PKGCONFIG=ON \
		-DOPENCV_ENABLE_PKG_CONFIG=ON \
		-DWITH_GTK=ON \
		-DWITH_GTK_2_X=OFF \
		-DWITH_QT=OFF \
		-DWITH_FFMPEG=OFF \
		-DWITH_GSTREAMER=OFF \
		-DWITH_V4L=ON \
		-DWITH_OPENCL=OFF \
		-DWITH_OPENGL=OFF \
		-DWITH_IPP=OFF \
		-DWITH_LAPACK=ON \
		-DWITH_OPENMP=ON \
		-DWITH_PROTOBUF=ON \
		-DBUILD_PROTOBUF=ON \
		-DWITH_JPEG=ON \
		-DWITH_PNG=ON \
		-DWITH_TIFF=OFF \
		-DWITH_WEBP=OFF \
		-DWITH_OPENJPEG=OFF \
		-DWITH_OPENEXR=OFF \
		-DWITH_AVIF=OFF \
		-DWITH_JPEGXL=OFF \
		-DWITH_EIGEN=OFF \
		-DWITH_TBB=OFF \
		-DWITH_ITT=OFF
	cmake --build "$work/opencv-build" --parallel "$(nproc)"
	cmake --install "$work/opencv-build"
	test -s "$PREFIX/lib/pkgconfig/opencv5.pc" || die "opencv5.pc was not installed"
	fix_pkgconfig_prefix "$PREFIX/lib/pkgconfig/opencv5.pc"
	satisfied opencv5 "$OPENCV_MINIMUM" || die "built OpenCV does not satisfy >= ${OPENCV_MINIMUM}"
}

libs() {
	local work
	work=$(mktemp -d)
	# Expand now: $work is out of scope when the EXIT trap runs.
	# shellcheck disable=SC2064
	trap "rm -rf '$work'" EXIT

	if satisfied INIReader "$INIREADER_MINIMUM"; then
		log "distribution INIReader $(pkg-config --modversion INIReader) is sufficient"
	else
		build_inih "$work"
	fi
	if satisfied yyjson "$YYJSON_MINIMUM"; then
		log "distribution yyjson $(pkg-config --modversion yyjson) is sufficient"
	else
		build_yyjson "$work"
	fi
	if satisfied opencv5 "$OPENCV_MINIMUM"; then
		log "distribution OpenCV $(pkg-config --modversion opencv5) is sufficient"
	else
		build_opencv "$work"
	fi

	# Register with the dynamic loader instead of exporting LD_LIBRARY_PATH, which
	# setuid helpers ignore.
	if [ -d "$PREFIX/lib" ]; then
		printf '%s\n' "$PREFIX/lib" > /etc/ld.so.conf.d/howdy-deps.conf
		ldconfig
	fi
	log "INIReader $(pkg-config --modversion INIReader), yyjson $(pkg-config --modversion yyjson), opencv5 $(pkg-config --modversion opencv5)"
}

models() {
	test -r "$MANIFEST" || die "model manifest not found: $MANIFEST"
	local joined
	# Join adjacent string literals split across lines, then pull out each field.
	joined=$(tr '\n\t' '  ' < "$MANIFEST" | sed -E 's/"[[:space:]]+"//g')

	local -a filenames urls hashes
	mapfile -t filenames < <(grep -oE '\.filename = "[^"]+"' <<< "$joined" | sed -E 's/^[^"]*"(.*)"$/\1/')
	mapfile -t urls < <(grep -oE '\.url = "[^"]+"' <<< "$joined" | sed -E 's/^[^"]*"(.*)"$/\1/')
	mapfile -t hashes < <(grep -oE '\.sha256 = "[^"]+"' <<< "$joined" | sed -E 's/^[^"]*"(.*)"$/\1/')
	[ "${#filenames[@]}" -ge 1 ] || die "no models found in $MANIFEST"
	[ "${#filenames[@]}" -eq "${#urls[@]}" ] && [ "${#urls[@]}" -eq "${#hashes[@]}" ] \
		|| die "model manifest fields do not line up"

	install -d -m 0755 "$PREFIX/models"
	local i
	for i in "${!filenames[@]}"; do
		log "downloading ${filenames[i]}"
		fetch "${urls[i]}" "${hashes[i]}" "$PREFIX/models/${filenames[i]}"
	done
	chmod 0644 "$PREFIX"/models/*
}

case "${1:-}" in
	libs) libs ;;
	models) models ;;
	*) die "usage: $0 libs|models" ;;
esac
