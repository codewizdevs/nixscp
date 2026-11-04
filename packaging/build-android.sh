#!/bin/bash
# The release APK for phones and tablets (arm64-v8a, Android 9+):
# bin/NixSCP-<version>-android-arm64.apk, signed with the release key.
# Needs packaging/android/setup-sdk.sh and build-deps.sh first.
#
# The key never goes into the repository:
#   NIXSCP_ANDROID_KEYSTORE        the keystore (.jks)
#   NIXSCP_ANDROID_KEY_ALIAS       its key (default: nixscp)
#   NIXSCP_ANDROID_KEYSTORE_PASS   the keystore's password (the key's too)
# Android installs an update only when it is signed with the same key: keep
# the keystore and its password safe (Google Play takes the same key later).
set -euo pipefail

SOURCE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$SOURCE_DIR"
: "${NIXSCP_ANDROID_KEYSTORE:?set NIXSCP_ANDROID_KEYSTORE to the release keystore}"
: "${NIXSCP_ANDROID_KEYSTORE_PASS:?set NIXSCP_ANDROID_KEYSTORE_PASS}"
ALIAS="${NIXSCP_ANDROID_KEY_ALIAS:-nixscp}"
# shellcheck disable=SC1091
source "${ANDROID_SDK_DIR:-$SOURCE_DIR/android-sdk}/env.sh"

VERSION=$(sed -n 's/^project(nixscp VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
BUILD=builds/android-arm64-release
"$QT_ANDROID_ARM64/bin/qt-cmake" -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DANDROID_SDK_ROOT="$ANDROID_SDK_ROOT" -DANDROID_NDK_ROOT="$ANDROID_NDK_ROOT" -DQT_HOST_PATH="$QT_HOST_PATH" \
  -DNIXSCP_BUILD_TESTS=OFF >/dev/null
cmake --build "$BUILD" --target apk >/dev/null
UNSIGNED=$(find "$BUILD" -name '*release-unsigned.apk' | head -1)

TOOLS="$ANDROID_HOME/build-tools/35.0.0"
OUT="bin/NixSCP-$VERSION-android-arm64.apk"
mkdir -p bin
"$TOOLS/zipalign" -f -p 4 "$UNSIGNED" "$OUT.aligned"
"$TOOLS/apksigner" sign --ks "$NIXSCP_ANDROID_KEYSTORE" --ks-key-alias "$ALIAS" \
  --ks-pass env:NIXSCP_ANDROID_KEYSTORE_PASS --key-pass env:NIXSCP_ANDROID_KEYSTORE_PASS \
  --out "$OUT" "$OUT.aligned"
rm -f "$OUT.aligned" "$OUT.idsig"
"$TOOLS/apksigner" verify "$OUT"
echo "build-android: $OUT"
