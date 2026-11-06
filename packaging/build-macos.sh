#!/bin/bash
# Builds the macOS app (universal: Apple Silicon and Intel) and its disk image:
#   bin/macos/NixSCP.app
#   bin/macos/NixSCP-<version>-macos-universal.dmg
#
# Needs: Xcode command line tools, CMake, Ninja, Qt 6 for macOS (universal,
# e.g. aqt install-qt mac desktop 6.8.3 clang_64) in QT_DIR, and OpenSSL 3 and
# libssh 0.11 built for both architectures with pkg-config files in
# DEPS/lib/pkgconfig (see .github/workflows/release.yml).
#
# The app is ad-hoc signed only (not notarized): on first start macOS asks to
# confirm it (right-click > Open, or System Settings > Privacy & Security).
set -euo pipefail
trap 'echo "build-macos: failed at line $LINENO: $BASH_COMMAND" >&2' ERR

SOURCE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$SOURCE_DIR/builds/macos}"
OUT_DIR="${OUT_DIR:-$SOURCE_DIR/bin/macos}"
: "${QT_DIR:?set QT_DIR to the Qt for macOS directory (…/6.8.3/macos)}"
: "${DEPS:?set DEPS to the OpenSSL/libssh prefix}"
ARCHS="${ARCHS:-arm64;x86_64}"
MIN_MACOS="${MIN_MACOS:-12.0}"
BUILD_TESTS="${BUILD_TESTS:-OFF}"
VERSION="$(sed -n 's/^project(nixscp VERSION \([0-9.]*\).*/\1/p' "$SOURCE_DIR/CMakeLists.txt")"

export PKG_CONFIG_PATH="$DEPS/lib/pkgconfig"
cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$QT_DIR" -DCMAKE_OSX_ARCHITECTURES="$ARCHS" -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_MACOS" \
  -DNIXSCP_WINSCP_ICONS=OFF -DNIXSCP_BUILD_TESTS="$BUILD_TESTS"
cmake --build "$BUILD_DIR"
echo "build-macos: built; assembling the bundle"
ls -la "$BUILD_DIR/NixSCP.app/Contents/MacOS" "$BUILD_DIR/nixscp-cli"

rm -rf "$OUT_DIR/NixSCP.app"
mkdir -p "$OUT_DIR"
cp -R "$BUILD_DIR/NixSCP.app" "$OUT_DIR/"
App="$OUT_DIR/NixSCP.app"
# the console client next to the GUI program (agents: nixscp --list etc. is
# the GUI program itself, Contents/MacOS/NixSCP)
cp "$BUILD_DIR/nixscp-cli" "$App/Contents/MacOS/"

# Application icon from the NixSCP artwork
IconSet="$BUILD_DIR/NixSCP.iconset"
rm -rf "$IconSet" && mkdir -p "$IconSet"
Icons="$SOURCE_DIR/resources/appicon"
for S in 16 32 128 256 512; do
  cp "$Icons/nixscp-$S.png" "$IconSet/icon_${S}x${S}.png"
done
cp "$Icons/nixscp-32.png" "$IconSet/icon_16x16@2x.png"
cp "$Icons/nixscp-64.png" "$IconSet/icon_32x32@2x.png"
cp "$Icons/nixscp-256.png" "$IconSet/icon_128x128@2x.png"
cp "$Icons/nixscp-512.png" "$IconSet/icon_256x256@2x.png"
mkdir -p "$App/Contents/Resources"
iconutil -c icns -o "$App/Contents/Resources/NixSCP.icns" "$IconSet"

echo "build-macos: icon done; deploying Qt and libraries"
# Qt frameworks and plugins, OpenSSL and libssh into the bundle
"$QT_DIR/bin/macdeployqt" "$App" -executable="$App/Contents/MacOS/nixscp-cli" -always-overwrite
# Qt's OpenSSL backend finds libssl in Frameworks when the first TLS socket opens,
# which macdeployqt cannot see. Without it Qt falls back to Secure Transport: no
# TLS 1.3, and no session reuse, which FTPS servers ask of data connections
Frameworks="$App/Contents/Frameworks"
cp -L "$DEPS/lib/libssl.3.dylib" "$Frameworks/libssl.3.dylib"
chmod u+w "$Frameworks/libssl.3.dylib"
install_name_tool -id @executable_path/../Frameworks/libssl.3.dylib "$Frameworks/libssl.3.dylib"
install_name_tool -change "$DEPS/lib/libcrypto.3.dylib" @executable_path/../Frameworks/libcrypto.3.dylib "$Frameworks/libssl.3.dylib"

# Checks: every binary universal, no library outside the bundle and the system
Failed=0
while IFS= read -r -d '' F; do
  if file "$F" | grep -q 'Mach-O'; then
    # The search paths of the build machine are searched on every Mac too, first
    # (/Users/runner/...): only paths relative to the bundle stay
    while read -r Path; do
      case "$Path" in
        @*) ;;
        *) install_name_tool -delete_rpath "$Path" "$F" || echo "build-macos: $F keeps the search path $Path" ;;
      esac
    done < <(otool -l "$F" | awk '$1 == "cmd" { Rpath = ($2 == "LC_RPATH") } Rpath && $1 == "path" { print $2 }' | sort -u)
    Archs="$(lipo -archs "$F" 2>/dev/null || true)"
    for A in ${ARCHS//;/ }; do
      if [[ " $Archs " != *" $A "* ]]; then
        echo "build-macos: $F lacks $A ($Archs)"; Failed=1
      fi
    done
    # otool -L: a "<file> (architecture ...):" header per slice, the
    # library's own install name, then what it links
    Id="$(otool -D "$F" 2>/dev/null | grep -v ':$' | head -1 || true)"
    while read -r Lib; do
      [ "$Lib" = "$Id" ] && continue
      case "$Lib" in
        /System/*|/usr/lib/*|@rpath/*|@executable_path/*|@loader_path/*) ;;
        *) echo "build-macos: $F links $Lib (outside the bundle)"; Failed=1 ;;
      esac
    done < <(otool -L "$F" | grep -v ':$' | awk '{print $1}')
  fi
done < <(find "$App/Contents" -type f -print0)
[ "$Failed" = 0 ] || { echo "build-macos: bundle check failed"; exit 1; }
echo "build-macos: all binaries contain ${ARCHS//;/ + } and link only bundled or system libraries"

# Ad-hoc signature (required on Apple Silicon; no Developer ID yet)
codesign --force --deep --sign - "$App"
codesign --verify --deep --strict "$App"

# What the backend above is for: the app must not end up on the other one
"$App/Contents/MacOS/nixscp-cli" /ini=nul /info | grep -q 'TLS/SSL implementation: OpenSSL' ||
  { echo "build-macos: the app does not load the bundled OpenSSL for TLS"; exit 1; }

# Disk image: the app, a link to /Applications and first-start notes
Stage="$BUILD_DIR/dmg"
rm -rf "$Stage" && mkdir -p "$Stage"
cp -R "$App" "$Stage/"
ln -s /Applications "$Stage/Applications"
cat > "$Stage/READ ME FIRST.txt" <<EOF
NixSCP $VERSION for macOS (Apple Silicon and Intel, macOS $MIN_MACOS or newer)

1. Drag NixSCP to Applications.
2. This build is not signed with an Apple Developer ID yet, so macOS blocks
   the first start. Either right-click NixSCP in Applications > Open > Open,
   or open it once, then System Settings > Privacy & Security > "Open Anyway".
   From a terminal you can also run:
     xattr -dr com.apple.quarantine /Applications/NixSCP.app
   (Copy NixSCP with Finder, not cp: otherwise macOS runs it from a
   temporary read-only copy, "App Translocation", until this is done.)

Command line (Agent Access, scripting):
  sudo ln -sf /Applications/NixSCP.app/Contents/MacOS/NixSCP /usr/local/bin/nixscp
  sudo ln -sf /Applications/NixSCP.app/Contents/MacOS/nixscp-cli /usr/local/bin/nixscp-cli

Settings: ~/Library/Application Support/NixSCP
Documentation: https://nixscp.com/docs
EOF
Dmg="$OUT_DIR/NixSCP-$VERSION-macos-universal.dmg"
rm -f "$Dmg"
hdiutil create -volname "NixSCP $VERSION" -srcfolder "$Stage" -ov -format UDZO "$Dmg" >/dev/null
ls -l "$Dmg"
