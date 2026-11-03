#!/bin/bash
# Sets up everything needed to build and test NixSCP for Android, without
# root, in android-sdk/ (ignored by git):
#   JDK 17, Android command line tools, platform, build tools, NDK, emulator
#   and an x86_64 system image, Qt for Android (arm64-v8a, x86_64) and the
#   matching desktop Qt (its host tools), and an emulator device "nixscp".
# Accepts the Android SDK licenses. Safe to run again (skips what exists).
set -euo pipefail

SOURCE_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
SDK="${ANDROID_SDK_DIR:-$SOURCE_DIR/android-sdk}"
QT_VERSION="${QT_VERSION:-6.8.3}"
API=35
BUILD_TOOLS=35.0.0
NDK=26.1.10909125           # r26b, the NDK Qt 6.8 is built with
JDK_URL="https://github.com/adoptium/temurin17-binaries/releases/download/jdk-17.0.13%2B11/OpenJDK17U-jdk_x64_linux_hotspot_17.0.13_11.tar.gz"
TOOLS_URL="https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip"

mkdir -p "$SDK"
cd "$SDK"

if [ ! -x jdk/bin/java ]; then
  echo "setup: JDK 17"
  curl -fsSL -o jdk.tar.gz "$JDK_URL"
  mkdir -p jdk && tar xf jdk.tar.gz -C jdk --strip-components=1 && rm jdk.tar.gz
fi
export JAVA_HOME="$SDK/jdk"
export ANDROID_HOME="$SDK/android"
export PATH="$JAVA_HOME/bin:$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools:$ANDROID_HOME/emulator:$PATH"

if [ ! -x "$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" ]; then
  echo "setup: Android command line tools"
  mkdir -p "$ANDROID_HOME/cmdline-tools"
  curl -fsSL -o tools.zip "$TOOLS_URL"
  unzip -q tools.zip -d "$ANDROID_HOME/cmdline-tools" && mv "$ANDROID_HOME/cmdline-tools/cmdline-tools" "$ANDROID_HOME/cmdline-tools/latest"
  rm tools.zip
fi

echo "setup: Android SDK packages (licenses accepted)"
yes | sdkmanager --licenses >/dev/null 2>&1 || true
sdkmanager --install "platform-tools" "platforms;android-$API" "build-tools;$BUILD_TOOLS" "ndk;$NDK" \
  "emulator" "system-images;android-$API;google_apis;x86_64" >/dev/null

if [ ! -d "qt/$QT_VERSION/android_arm64_v8a" ] || [ ! -d "qt/$QT_VERSION/android_x86_64" ] || [ ! -d "qt/$QT_VERSION/gcc_64" ]; then
  echo "setup: Qt $QT_VERSION for Android"
  # aqtinstall: $AQT, an installed aqt, or a private venv (Ubuntu's python3
  # may lack ensurepip: then pip comes from get-pip.py)
  AQT="${AQT:-$(command -v aqt || true)}"
  if [ -z "$AQT" ]; then
    if [ ! -x aqt-venv/bin/aqt ]; then
      rm -rf aqt-venv
      python3 -m venv aqt-venv 2>/dev/null || { python3 -m venv --without-pip aqt-venv \
        && curl -fsSL https://bootstrap.pypa.io/get-pip.py | aqt-venv/bin/python3 - --quiet; }
      aqt-venv/bin/python3 -m pip install --quiet aqtinstall
    fi
    AQT="$SDK/aqt-venv/bin/aqt"
  fi
  "$AQT" install-qt linux desktop "$QT_VERSION" linux_gcc_64 -O qt >/dev/null
  for A in android_arm64_v8a android_x86_64; do
    "$AQT" install-qt linux android "$QT_VERSION" "$A" -O qt >/dev/null
  done
fi

if ! avdmanager list avd 2>/dev/null | grep -q "Name: nixscp"; then
  echo "setup: emulator device nixscp"
  echo no | avdmanager create avd -n nixscp -k "system-images;android-$API;google_apis;x86_64" -d pixel_7 >/dev/null
fi

cat > "$SDK/env.sh" <<EOF
# source this file: the Android build environment of NixSCP
export JAVA_HOME="$SDK/jdk"
export ANDROID_HOME="$SDK/android"
export ANDROID_SDK_ROOT="$SDK/android"
export ANDROID_NDK_ROOT="$SDK/android/ndk/$NDK"
export QT_ANDROID_ARM64="$SDK/qt/$QT_VERSION/android_arm64_v8a"
export QT_ANDROID_X86_64="$SDK/qt/$QT_VERSION/android_x86_64"
export QT_HOST_PATH="$SDK/qt/$QT_VERSION/gcc_64"
export PATH="\$JAVA_HOME/bin:\$ANDROID_HOME/platform-tools:\$ANDROID_HOME/emulator:\$ANDROID_HOME/cmdline-tools/latest/bin:\$PATH"
EOF
echo "setup: done. Environment: source $SDK/env.sh"
