#!/usr/bin/env bash
# Builds a NixSCP AppImage with linuxdeploy and linuxdeploy-plugin-qt.
#
# Usage: packaging/build-appimage.sh [output-dir]
#
# Environment:
#   BUILD_DIR      build directory (default: <repo>/build-appimage)
#   CACHE_DIR      where the linuxdeploy tools are downloaded
#                  (default: ${XDG_CACHE_HOME:-~/.cache}/nixscp-packaging)
#   QMAKE          Qt 6 qmake used by the Qt plugin (default: autodetected)
#   JOBS           parallel build jobs (default: nproc)
#   CMAKE_ARGS     extra arguments for the cmake configure step
#   UPDATE_TOOLS=1 re-download linuxdeploy even when cached
#   EXTRA_QT_PLUGINS, EXTRA_PLATFORM_PLUGINS are passed through to the Qt plugin
set -euo pipefail

SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTPUT_DIR="$(realpath -m "${1:-$PWD}")"
BUILD_DIR="$(realpath -m "${BUILD_DIR:-$SOURCE_DIR/build-appimage}")"
CACHE_DIR="$(realpath -m "${CACHE_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/nixscp-packaging}")"
JOBS="${JOBS:-$(nproc)}"
ARCH="$(uname -m)"
APPDIR="$BUILD_DIR/AppDir"

die()
{
  echo "build-appimage: $*" >&2
  exit 1
}

for Tool in cmake curl; do
  command -v "$Tool" >/dev/null || die "'$Tool' is required"
done

# linuxdeploy publishes only x86_64, i386, aarch64 and armhf builds
case "$ARCH" in
  x86_64|i386|aarch64|armhf) ;;
  i686) ARCH=i386 ;;
  armv7l) ARCH=armhf ;;
  *) die "unsupported architecture $ARCH" ;;
esac

# Qt 6 qmake for linuxdeploy-plugin-qt (it locates plugins and libraries through it)
if [ -z "${QMAKE:-}" ]; then
  for Candidate in qmake6 /usr/lib/qt6/bin/qmake "/usr/lib/$(uname -m)-linux-gnu/qt6/bin/qmake" qmake; do
    if command -v "$Candidate" >/dev/null 2>&1 && \
       "$Candidate" -query QT_VERSION 2>/dev/null | grep -q '^6\.'; then
      QMAKE="$(command -v "$Candidate")"
      break
    fi
  done
fi
[ -n "${QMAKE:-}" ] || die "Qt 6 qmake not found (install qt6-base-dev-tools or set QMAKE)"
export QMAKE

# Download the tools into the cache
mkdir -p "$CACHE_DIR"
fetch()
{
  local Url="$1" File="$CACHE_DIR/$2"
  if [ ! -s "$File" ] || [ "${UPDATE_TOOLS:-0}" = 1 ]; then
    echo "Downloading $Url"
    curl -fL --retry 3 -o "$File.part" "$Url" || die "download failed: $Url"
    mv "$File.part" "$File"
  fi
  chmod +x "$File"
}
fetch "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-$ARCH.AppImage" \
  "linuxdeploy-$ARCH.AppImage"
fetch "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-$ARCH.AppImage" \
  "linuxdeploy-plugin-qt-$ARCH.AppImage"
# linuxdeploy finds plugins named linuxdeploy-plugin-<name>*.AppImage in PATH or next to itself
export PATH="$CACHE_DIR:$PATH"

# The linuxdeploy AppImages need libfuse2 to mount themselves; extracting them
# instead works everywhere (containers, CI, systems with only fuse3)
export APPIMAGE_EXTRACT_AND_RUN="${APPIMAGE_EXTRACT_AND_RUN:-1}"

# Build and install into the AppDir
# shellcheck disable=SC2086
cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr ${CMAKE_ARGS:-}
cmake --build "$BUILD_DIR" -j "$JOBS" --target nixscp nixscp-cli
rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD_DIR" --strip

# The desktop file references Icon=nixscp; an AppImage cannot be built without it
Icon="$APPDIR/usr/share/icons/hicolor/256x256/apps/nixscp.png"
if [ ! -f "$Icon" ]; then
  [ -f "$SOURCE_DIR/data/nixscp.png" ] || die "data/nixscp.png is missing (re-run cmake after adding it)"
  install -Dm644 "$SOURCE_DIR/data/nixscp.png" "$Icon"
fi

Executables=()
for Binary in nixscp nixscp-cli; do
  [ -x "$APPDIR/usr/bin/$Binary" ] && Executables+=(--executable "$APPDIR/usr/bin/$Binary")
done

mkdir -p "$OUTPUT_DIR"
cd "$OUTPUT_DIR"
VERSION="$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$BUILD_DIR/CMakeCache.txt")"
export LINUXDEPLOY_OUTPUT_VERSION="${VERSION:-0}"
"$CACHE_DIR/linuxdeploy-$ARCH.AppImage" \
  --appdir "$APPDIR" \
  "${Executables[@]}" \
  --desktop-file "$APPDIR/usr/share/applications/nixscp.desktop" \
  --icon-file "$Icon" \
  --plugin qt \
  --output appimage

echo "AppImage written to $OUTPUT_DIR:"
ls -1 "$OUTPUT_DIR"/NixSCP-*.AppImage
