#!/usr/bin/env bash
# Builds switch-mesa as devkitPro's package does (switch/mesa in
# github.com/devkitPro/pacman-packages, pinned below, with its checksums),
# with this folder's patches on top, into build/switch/mesa/install. The
# Switch host links that Mesa when it is there (port/switch/host/Makefile).
#
#     bash port/switch/mesa/build_mesa.sh            # fetch, patch, build
#     bash port/switch/mesa/build_mesa.sh --source   # fetch and patch only
#
# Needs devkitPro (switch-dev, switch-libdrm_nouveau, dkp-meson-scripts,
# switch-pkg-config, dkp-toolchain-vars), meson, ninja, bison, flex and
# Python's mako. Refer to port/switch/mesa/README.md.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
OUT="$ROOT/build/switch/mesa"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITPRO

RECIPE_COMMIT=c7876f72c1f3353725ab8468f0ef9180280a8c31
RECIPE="https://raw.githubusercontent.com/devkitPro/pacman-packages/$RECIPE_COMMIT/switch/mesa"
MESA=mesa-20.1.0-rc3
# (the PKGBUILD's address, mesa.freedesktop.org/archive, now keeps 20.x releases
# under older-versions)
MESA_URL="https://archive.mesa3d.org/older-versions/20.x/$MESA.tar.xz"

# name, URL, SHA-256 (the PKGBUILD's)
DOWNLOADS=(
	"$MESA.tar.xz" "$MESA_URL" c90b75ea34302ebde9b81b87c5642fa864c40fe9c4ad34ce0793170c1413168d
	"switch-mesa-20.1.0-5.patch" "$RECIPE/switch-mesa-20.1.0-5.patch" 950f93d3e5b6ae9c5a42c2918623fe9a80d7ee398d92d2e73abec86b62d75916
	"gl_XML.py.patch" "$RECIPE/gl_XML.py.patch" a9bc326195b3fe29709e079466a8b2162a2ac9409f694eaad5494490155e2dd4
	"glX_XML.py.patch" "$RECIPE/glX_XML.py.patch" 1475defcdf8600690eaddeee28d8f01310635c1273fb541f668e8789714d36ca
)

mkdir -p "$OUT/download"
for ((i = 0; i < ${#DOWNLOADS[@]}; i += 3)); do
	name="${DOWNLOADS[i]}" url="${DOWNLOADS[i+1]}" sum="${DOWNLOADS[i+2]}"
	file="$OUT/download/$name"
	if [ ! -f "$file" ] || ! echo "$sum  $file" | sha256sum -c --quiet - 2>/dev/null; then
		echo "downloading $name"
		curl -fsSL --retry 3 -o "$file.part" "$url"
		mv "$file.part" "$file"
	fi
	echo "$sum  $file" | sha256sum -c --quiet - || { echo "$name: wrong checksum" >&2; exit 1; }
done

# a fresh source tree each time, so that a patch is never applied twice
rm -rf "$OUT/src"
mkdir -p "$OUT/src"
tar -xJf "$OUT/download/$MESA.tar.xz" -C "$OUT/src"
SRC="$OUT/src/$MESA"
patch -d "$SRC" -p1 -s -i "$OUT/download/switch-mesa-20.1.0-5.patch"
patch -d "$SRC" -p1 -s -i "$OUT/download/gl_XML.py.patch"
patch -d "$SRC" -p1 -s -i "$OUT/download/glX_XML.py.patch"
for own in "$HERE"/*.patch; do
	[ -e "$own" ] || continue
	echo "applying $(basename "$own")"
	patch -d "$SRC" -p1 -s -i "$own"
done
if [ "${1:-}" = "--source" ]; then
	echo "patched source: $SRC"
	exit 0
fi

# as the PKGBUILD, installed under build/switch/mesa/install instead of the
# system's portlibs
rm -rf "$OUT/build" "$OUT/install"
(cd "$SRC" && "$DEVKITPRO/meson-cross.sh" switch "$OUT/crossfile.txt" "$OUT/build" -Db_ndebug=true)
ninja -C "$OUT/build"
DESTDIR="$OUT/install" ninja -C "$OUT/build" install
# (its licence, for the NRO's notices: tools/ci_build.py)
cp "$SRC/docs/license.html" "$OUT/install/mesa-license.html"
echo "Mesa installed under $OUT/install$DEVKITPRO/portlibs/switch"
